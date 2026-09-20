import queue
import threading
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext
from tkinter import ttk

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

if __package__:
    from .flash_protocol import (
        DEFAULT_FIRMWARE_FILE,
        FirmwareUpdateCancelled,
        flash_firmware,
    )
else:
    from flash_protocol import (
        DEFAULT_FIRMWARE_FILE,
        FirmwareUpdateCancelled,
        flash_firmware,
    )

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
