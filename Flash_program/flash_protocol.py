import math
import struct
import time
from pathlib import Path
from typing import Optional

try:
    import serial
except ImportError:
    serial = None

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
FINISHED_SEND_DELAY_SECONDS = 0.05
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

        _log(
            log_callback,
            f"Waiting {FINISHED_SEND_DELAY_SECONDS * 1000:.0f} ms before FINISHED",
        )
        time.sleep(FINISHED_SEND_DELAY_SECONDS)

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


