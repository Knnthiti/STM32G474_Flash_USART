import math
import queue
import struct
import threading
import time
from pathlib import Path
from typing import Optional
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext
from tkinter import ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

# ---------------------------------------------------------
# Commands & Constants
# ---------------------------------------------------------
PC_CMD_START = 0x08  # Initial command sent to STM32 to start update process
PC_CMD_SENDING = 0x68  # Command indicating data chunk transmission
PC_CMD_FINISHED = 0x9A  # Command indicating file transfer completion
PC_CMD_WAIT = 0x21  # Command sent when STM32 buffer is full to trigger flash write

STM_ACK_READY = 0x4A  # STM32 is ready to receive next packet
STM_ACK_PAUSE = 0x6A  # STM32 buffer is full, pause transmission
STM_ACK_FINISHED = 0x56  # STM32 successfully finished flashing
KNOWN_STM_ACKS = (STM_ACK_READY, STM_ACK_PAUSE, STM_ACK_FINISHED)

# Protocol configurations
FIRMWARE_VERSION = 0x67
PAYLOAD_SIZE = 1016
PACKET_SIZE = 1024
ERASED_FLASH_BYTE = 0xFF
DEFAULT_BAUDRATE = 115200
DEFAULT_FIRMWARE_FILE = Path(__file__).with_name("App.bin")
ACK_POLL_INTERVAL_SECONDS = 0.05
START_ACK_RETRIES = 6000
PACKET_ACK_RETRIES = 1000
FLASH_WRITE_ACK_RETRIES = 6000
FINISH_ACK_RETRIES = 6000


class FirmwareUpdateCancelled(Exception):
    pass


# ---------------------------------------------------------
# Function to simulate STM32 Hardware CRC32 calculation
# ---------------------------------------------------------
def software_crc32_c_style(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        word = int.from_bytes(data[i : i + 4], byteorder="little")
        crc ^= word
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


# ---------------------------------------------------------
# Packet Builder Helper (Includes Version Variable)
# ---------------------------------------------------------
def build_packet(command, payload_bytes=b"", pad_byte=0x00):
    """Construct a 1024-byte packet: Header(4) + Data(1016) + CRC(4)."""
    if len(payload_bytes) > PAYLOAD_SIZE:
        raise ValueError("Payload is larger than one STM32 packet.")
    if not 0 <= pad_byte <= 0xFF:
        raise ValueError("pad_byte must be in range 0..255.")

    header = struct.pack("<B B H", command, FIRMWARE_VERSION, PACKET_SIZE)
    padded_payload = payload_bytes.ljust(PAYLOAD_SIZE, bytes([pad_byte]))
    data_to_crc = header + padded_payload
    my_crc = software_crc32_c_style(data_to_crc)
    return data_to_crc + struct.pack("<I", my_crc)


def find_valid_packet_start(buffer: bytes) -> Optional[int]:
    max_start = len(buffer) - PACKET_SIZE
    for start in range(max_start + 1):
        if buffer[start] not in KNOWN_STM_ACKS:
            continue
        if buffer[start + 2] != 0x00 or buffer[start + 3] != 0x04:
            continue
        return start
    return None


# ---------------------------------------------------------
# Function to wait and receive responses from STM32
# ---------------------------------------------------------
def wait_for_stm32(
    ser,
    expected_acks,
    timeout_retries=1000,
    cancel_event=None,
    log_callback=None,
):
    """Wait for expected ACKs from STM32, reading 1024-byte CDC packets."""
    retries = 0
    rx_buffer = bytearray()

    while retries < timeout_retries:
        if cancel_event is not None and cancel_event.is_set():
            raise FirmwareUpdateCancelled()

        waiting = ser.in_waiting
        if waiting > 0:
            rx_buffer.extend(ser.read(waiting))

            while len(rx_buffer) >= PACKET_SIZE:
                packet_start = find_valid_packet_start(rx_buffer)
                if packet_start is None:
                    if len(rx_buffer) > PACKET_SIZE:
                        dropped = len(rx_buffer) - (PACKET_SIZE - 1)
                        del rx_buffer[:dropped]
                        if log_callback is not None:
                            log_callback(
                                f"[!] Dropped {dropped} byte(s) while searching for ACK frame"
                            )
                    break

                if packet_start > 0:
                    del rx_buffer[:packet_start]
                    if log_callback is not None:
                        log_callback(
                            f"[!] Ignored {packet_start} byte(s) before ACK frame"
                        )

                packet = bytes(rx_buffer[:PACKET_SIZE])
                del rx_buffer[:PACKET_SIZE]
                ack_cmd = packet[0]

                if ack_cmd in expected_acks:
                    return ack_cmd

                if log_callback is not None:
                    log_callback(f"[!] Unexpected ACK received: 0x{ack_cmd:02X}")

        time.sleep(ACK_POLL_INTERVAL_SECONDS)
        retries += 1

    return None


def send_packet(ser, packet: bytes):
    ser.write(packet)
    ser.flush()


def wait_until_stm32_ready(
    ser,
    timeout_retries=PACKET_ACK_RETRIES,
    cancel_event=None,
    log_callback=None,
    status_callback=None,
    pause_status="STM32 is writing flash",
    pause_log=None,
    timeout_message="Timeout while waiting for STM32 READY.",
):
    ack = wait_for_stm32(
        ser,
        [STM_ACK_READY, STM_ACK_PAUSE],
        timeout_retries=timeout_retries,
        cancel_event=cancel_event,
        log_callback=log_callback,
    )

    if ack == STM_ACK_PAUSE:
        _status(status_callback, pause_status)
        if pause_log is None:
            pause_log = (
                f"STM32 buffer is full (0x{STM_ACK_PAUSE:02X}), "
                f"sending WAIT (0x{PC_CMD_WAIT:02X})"
            )
        _log(log_callback, pause_log)
        send_packet(ser, build_packet(PC_CMD_WAIT))

        ack = wait_for_stm32(
            ser,
            [STM_ACK_READY],
            timeout_retries=FLASH_WRITE_ACK_RETRIES,
            cancel_event=cancel_event,
            log_callback=log_callback,
        )

        if ack != STM_ACK_READY:
            raise TimeoutError(timeout_message)

        _log(log_callback, f"Received READY (0x{STM_ACK_READY:02X}) after WAIT")

    if ack != STM_ACK_READY:
        raise TimeoutError(timeout_message)

    return ack


def flash_firmware(
    port,
    firmware_path,
    baudrate=DEFAULT_BAUDRATE,
    cancel_event=None,
    log_callback=None,
    status_callback=None,
    progress_callback=None,
):
    if serial is None:
        raise RuntimeError(
            "pyserial is not installed. Install it with: pip install pyserial"
        )

    firmware_path = Path(firmware_path)
    if not firmware_path.exists():
        raise FileNotFoundError(f"Firmware file not found: {firmware_path}")
    if firmware_path.suffix.lower() != ".bin":
        raise ValueError("Please select a .bin firmware file.")

    firmware_data = firmware_path.read_bytes()
    total_size = len(firmware_data)
    if total_size == 0:
        raise ValueError("Firmware file is empty.")

    total_chunks = math.ceil(total_size / PAYLOAD_SIZE)
    _log(
        log_callback,
        f"Loaded {firmware_path.name}: {total_size} bytes ({total_chunks} packets)",
    )

    if cancel_event is not None and cancel_event.is_set():
        raise FirmwareUpdateCancelled()

    ser = serial.Serial(port, baudrate, timeout=0.1, write_timeout=5)
    try:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        _status(status_callback, f"Connected to {port}")
        _log(log_callback, f"Connected to {port} at {baudrate} baud")

        _status(status_callback, "Sending start command")
        _log(
            log_callback,
            f"Sending START (0x{PC_CMD_START:02X}), firmware version 0x{FIRMWARE_VERSION:02X}",
        )
        send_packet(ser, build_packet(PC_CMD_START, struct.pack("<I", total_size)))

        for chunk_index in range(total_chunks):
            if cancel_event is not None and cancel_event.is_set():
                raise FirmwareUpdateCancelled()

            wait_until_stm32_ready(
                ser,
                timeout_retries=(
                    START_ACK_RETRIES if chunk_index == 0 else PACKET_ACK_RETRIES
                ),
                cancel_event=cancel_event,
                log_callback=log_callback,
                status_callback=status_callback,
                pause_status="STM32 is writing flash",
                pause_log=(
                    f"STM32 buffer is full (0x{STM_ACK_PAUSE:02X}), "
                    f"sending WAIT (0x{PC_CMD_WAIT:02X})"
                ),
                timeout_message=f"Timeout at packet {chunk_index + 1}/{total_chunks}.",
            )

            start_idx = chunk_index * PAYLOAD_SIZE
            end_idx = min(start_idx + PAYLOAD_SIZE, total_size)
            chunk_data = firmware_data[start_idx:end_idx]
            packet = build_packet(
                PC_CMD_SENDING,
                chunk_data,
                pad_byte=ERASED_FLASH_BYTE,
            )

            send_packet(ser, packet)
            _status(
                status_callback,
                f"Sending packet {chunk_index + 1}/{total_chunks}",
            )
            _log(
                log_callback,
                f"[{chunk_index + 1}/{total_chunks}] Sent {len(chunk_data)} bytes",
            )
            _progress(progress_callback, chunk_index + 1, total_chunks)

        _status(status_callback, "Waiting for final READY")
        _log(
            log_callback,
            "No firmware data remains. Waiting for READY before FINISHED "
            f"(0x{PC_CMD_FINISHED:02X})",
        )
        wait_until_stm32_ready(
            ser,
            timeout_retries=PACKET_ACK_RETRIES,
            cancel_event=cancel_event,
            log_callback=log_callback,
            status_callback=status_callback,
            pause_status="STM32 is writing final flash block",
            pause_log=(
                f"STM32 buffer is full (0x{STM_ACK_PAUSE:02X}), "
                f"sending final WAIT (0x{PC_CMD_WAIT:02X})"
            ),
            timeout_message="STM32 is not ready to accept FINISHED command.",
        )

        _log(log_callback, f"Sending FINISHED (0x{PC_CMD_FINISHED:02X})")
        send_packet(ser, build_packet(PC_CMD_FINISHED))
        _status(status_callback, "Waiting for final confirmation")
        _log(log_callback, f"Waiting for final success ACK (0x{STM_ACK_FINISHED:02X})")

        ack_finish = wait_for_stm32(
            ser,
            [STM_ACK_FINISHED],
            timeout_retries=FINISH_ACK_RETRIES,
            cancel_event=cancel_event,
            log_callback=log_callback,
        )
        if ack_finish != STM_ACK_FINISHED:
            raise TimeoutError("Failed to receive final confirmation from STM32.")

        _progress(progress_callback, total_chunks, total_chunks)
        _status(status_callback, "Success")
        _log(log_callback, "Success! Firmware successfully updated.")
    finally:
        if ser.is_open:
            ser.close()
            _log(log_callback, "Serial port closed")


def _log(callback, message):
    if callback is not None:
        callback(message)


def _status(callback, message):
    if callback is not None:
        callback(message)


def _progress(callback, done, total):
    if callback is not None:
        callback(done, total)


class FirmwareUpdaterGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("STM32G474 UART Firmware Updater")
        self.root.minsize(720, 460)

        self.message_queue = queue.Queue()
        self.worker_thread = None
        self.cancel_event = threading.Event()
        self.port_lookup = {}

        self.port_var = tk.StringVar()
        self.file_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Ready")
        self.progress_var = tk.DoubleVar(value=0)
        self.progress_text_var = tk.StringVar(value="0%")

        if DEFAULT_FIRMWARE_FILE.exists():
            self.file_var.set(str(DEFAULT_FIRMWARE_FILE))

        self._build_ui()
        self.refresh_ports()
        self.root.after(100, self._process_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self):
        main = ttk.Frame(self.root, padding=14)
        main.grid(row=0, column=0, sticky="nsew")

        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main.columnconfigure(1, weight=1)
        main.rowconfigure(5, weight=1)

        title = ttk.Label(
            main,
            text="STM32G474 UART Firmware Updater",
            font=("Segoe UI", 15, "bold"),
        )
        title.grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 14))

        ttk.Label(main, text="COM Port").grid(row=1, column=0, sticky="w", pady=5)
        self.port_combo = ttk.Combobox(main, textvariable=self.port_var, width=42)
        self.port_combo.grid(row=1, column=1, columnspan=2, sticky="ew", padx=(8, 8))
        self.refresh_button = ttk.Button(
            main,
            text="Refresh",
            command=self.refresh_ports,
        )
        self.refresh_button.grid(row=1, column=3, sticky="ew", pady=5)

        ttk.Label(main, text="Firmware .bin").grid(row=2, column=0, sticky="w", pady=5)
        self.file_entry = ttk.Entry(main, textvariable=self.file_var)
        self.file_entry.grid(row=2, column=1, columnspan=2, sticky="ew", padx=(8, 8))
        self.browse_button = ttk.Button(
            main,
            text="Browse",
            command=self.browse_file,
        )
        self.browse_button.grid(row=2, column=3, sticky="ew", pady=5)

        ttk.Label(main, text="Status").grid(row=3, column=0, sticky="w", pady=5)
        self.status_label = ttk.Label(main, textvariable=self.status_var)
        self.status_label.grid(row=3, column=1, columnspan=3, sticky="w", padx=(8, 0))

        self.progress_bar = ttk.Progressbar(
            main,
            variable=self.progress_var,
            maximum=100,
        )
        self.progress_bar.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(10, 8))
        self.progress_label = ttk.Label(
            main, textvariable=self.progress_text_var, width=8
        )
        self.progress_label.grid(row=4, column=3, sticky="e", pady=(10, 8))

        self.log_text = scrolledtext.ScrolledText(
            main,
            height=13,
            wrap="word",
            state="disabled",
            font=("Consolas", 10),
        )
        self.log_text.grid(row=5, column=0, columnspan=4, sticky="nsew", pady=(4, 10))

        button_row = ttk.Frame(main)
        button_row.grid(row=6, column=0, columnspan=4, sticky="ew")
        button_row.columnconfigure(0, weight=1)

        self.start_button = ttk.Button(
            button_row,
            text="Start Flash",
            command=self.start_flash,
        )
        self.start_button.grid(row=0, column=1, sticky="e", padx=(0, 8))

        self.reset_button = ttk.Button(
            button_row,
            text="Reset",
            command=self.reset_gui,
        )
        self.reset_button.grid(row=0, column=2, sticky="e")

    def refresh_ports(self):
        current = self.port_var.get()
        self.port_lookup = {}
        values = []

        if list_ports is not None:
            for port in list_ports.comports():
                label = port.device
                if port.description and port.description != "n/a":
                    label = f"{port.device} - {port.description}"
                self.port_lookup[label] = port.device
                values.append(label)

        self.port_combo["values"] = values

        if current in values:
            self.port_var.set(current)
        elif values:
            self.port_var.set(values[0])
        elif not current:
            self.port_var.set("")

        if not values:
            self._append_log(
                "No COM ports found. Click Refresh after connecting STM32."
            )

    def browse_file(self):
        initial_dir = (
            Path(self.file_var.get()).parent if self.file_var.get() else Path.cwd()
        )
        file_path = filedialog.askopenfilename(
            title="Select firmware .bin file",
            initialdir=initial_dir,
            filetypes=[("Binary firmware", "*.bin"), ("All files", "*.*")],
        )
        if file_path:
            self.file_var.set(file_path)
            self._append_log(f"Selected firmware: {file_path}")

    def start_flash(self):
        if self._is_busy():
            return

        port = self._selected_port()
        firmware_file = self.file_var.get().strip()

        if not port:
            messagebox.showerror("Missing COM Port", "Please select a COM port.")
            return
        if not firmware_file:
            messagebox.showerror("Missing Firmware", "Please select a .bin file.")
            return

        firmware_path = Path(firmware_file)
        if firmware_path.suffix.lower() != ".bin":
            messagebox.showerror("Invalid Firmware", "Please select a .bin file.")
            return
        if not firmware_path.exists():
            messagebox.showerror("File Not Found", f"File not found:\n{firmware_path}")
            return

        self.cancel_event.clear()
        self._set_busy(True)
        self._set_progress(0, 1)
        self.status_var.set("Starting")
        self._append_log("")
        self._append_log("--- Starting firmware update ---")

        self.worker_thread = threading.Thread(
            target=self._worker,
            args=(port, firmware_path),
            daemon=True,
        )
        self.worker_thread.start()

    def reset_gui(self):
        if self._is_busy():
            self.cancel_event.set()
            self.status_var.set("Reset requested")
            self._append_log("Reset requested. Stopping current operation...")
            return

        self.status_var.set("Ready")
        self._set_progress(0, 1)
        self._clear_log()
        if DEFAULT_FIRMWARE_FILE.exists():
            self.file_var.set(str(DEFAULT_FIRMWARE_FILE))
        self.refresh_ports()

    def _worker(self, port, firmware_path):
        try:
            flash_firmware(
                port=port,
                firmware_path=firmware_path,
                cancel_event=self.cancel_event,
                log_callback=lambda message: self.message_queue.put(("log", message)),
                status_callback=lambda message: self.message_queue.put(
                    ("status", message)
                ),
                progress_callback=lambda done, total: self.message_queue.put(
                    ("progress", done, total)
                ),
            )
            self.message_queue.put(("done", True, "Firmware update completed."))
        except FirmwareUpdateCancelled:
            self.message_queue.put(("done", False, "Operation reset by user."))
        except Exception as exc:
            self.message_queue.put(("error", str(exc)))
            self.message_queue.put(("done", False, "Firmware update failed."))

    def _process_queue(self):
        try:
            while True:
                item = self.message_queue.get_nowait()
                kind = item[0]

                if kind == "log":
                    self._append_log(item[1])
                elif kind == "status":
                    self.status_var.set(item[1])
                elif kind == "progress":
                    self._set_progress(item[1], item[2])
                elif kind == "error":
                    self._append_log(f"Error: {item[1]}")
                    messagebox.showerror("Firmware Update Error", item[1])
                elif kind == "done":
                    success, message = item[1], item[2]
                    self.status_var.set("Success" if success else message)
                    self._append_log(message)
                    self._set_busy(False)
        except queue.Empty:
            pass

        self.root.after(100, self._process_queue)

    def _selected_port(self):
        selected = self.port_var.get().strip()
        if not selected:
            return ""
        return self.port_lookup.get(selected, selected.split()[0])

    def _set_busy(self, busy):
        state = "disabled" if busy else "normal"
        self.start_button["state"] = state
        self.refresh_button["state"] = state
        self.browse_button["state"] = state
        self.port_combo["state"] = state
        self.file_entry["state"] = state

    def _set_progress(self, done, total):
        total = max(total, 1)
        percent = min(max((done / total) * 100, 0), 100)
        self.progress_var.set(percent)
        self.progress_text_var.set(f"{percent:.0f}%")

    def _append_log(self, message):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", f"{message}\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def _is_busy(self):
        return self.worker_thread is not None and self.worker_thread.is_alive()

    def _on_close(self):
        if self._is_busy():
            should_close = messagebox.askyesno(
                "Update Running",
                "Firmware update is running. Stop it and close the program?",
            )
            if not should_close:
                return
            self.cancel_event.set()
        self.root.destroy()


def main():
    root = tk.Tk()
    FirmwareUpdaterGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
