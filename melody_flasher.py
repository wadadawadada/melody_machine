#!/usr/bin/env python3
"""Melody Machine one-file GUI flasher.

Features:
- Auto-detect serial ports
- Auto-select firmware from project root (prefers melody_machine.bin)
- Flash ESP32-S3 via esptool with progress + live logs
"""

from __future__ import annotations

import glob
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk


ROOT_DIR = Path(__file__).resolve().parent
DEFAULT_FW_NAME = "melody_machine.bin"
PERCENT_RE = re.compile(r"(\d+(?:\.\d+)?)%")


@dataclass
class PortInfo:
    device: str
    label: str


def detect_ports() -> list[PortInfo]:
    ports: list[PortInfo] = []

    try:
        from serial.tools import list_ports  # type: ignore

        for p in sorted(list_ports.comports(), key=lambda x: x.device):
            desc = p.description or "Serial device"
            ports.append(PortInfo(device=p.device, label=f"{p.device} - {desc}"))
        if ports:
            return ports
    except Exception:
        pass

    if os.name == "nt":
        try:
            cmd = [
                "powershell",
                "-NoProfile",
                "-Command",
                "Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Name",
            ]
            out = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
            for line in out.splitlines():
                line = line.strip()
                if not line.startswith("COM"):
                    continue
                parts = line.split(None, 1)
                dev = parts[0]
                name = parts[1] if len(parts) > 1 else "Serial device"
                ports.append(PortInfo(device=dev, label=f"{dev} - {name}"))
        except Exception:
            pass

    return ports


def find_default_firmware() -> Path | None:
    preferred = ROOT_DIR / DEFAULT_FW_NAME
    if preferred.exists():
        return preferred

    bins = sorted(ROOT_DIR.glob("*.bin"))
    if not bins:
        return None

    for name in ("melody_machine.bin", "flashed_app_full.bin", "flashed_app.bin"):
        p = ROOT_DIR / name
        if p.exists():
            return p

    return bins[0]


def find_esptool_command() -> tuple[list[str] | None, str]:
    if shutil.which("esptool.py"):
        return (["esptool.py"], "esptool.py (PATH)")
    if shutil.which("esptool"):
        return (["esptool"], "esptool (PATH)")

    try:
        import esptool  # type: ignore # noqa: F401

        return ([sys.executable, "-m", "esptool"], "python -m esptool")
    except Exception:
        pass

    candidates = []
    if os.name == "nt":
        local = Path(os.environ.get("LOCALAPPDATA", "")) / "Arduino15" / "packages" / "esp32" / "tools" / "esptool_py"
        candidates.extend(glob.glob(str(local / "*" / "esptool.py")))

    bb = ROOT_DIR.parent / "blackbox_wallet"
    candidates.extend(glob.glob(str(bb / "**" / "esptool.py"), recursive=True))

    for c in candidates:
        p = Path(c)
        if p.exists():
            return ([sys.executable, str(p)], str(p))

    return (None, "not found")


class FlasherApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Melody Machine Flasher")
        self.root.geometry("980x680")
        self.root.minsize(860, 620)

        self.log_queue: queue.Queue[str] = queue.Queue()
        self.worker: threading.Thread | None = None
        self.stop_event = threading.Event()

        self._build_style()
        self._build_ui()
        self._init_defaults()
        self._poll_logs()

    def _build_style(self) -> None:
        style = ttk.Style(self.root)
        try:
            # Prefer a square, non-rounded native look.
            style.theme_use("classic")
        except tk.TclError:
            pass

        bg = "#0f1220"
        panel = "#171a2b"
        text = "#e9ecff"
        accent = "#4f8cff"

        self.root.configure(bg=bg)
        style.configure("TFrame", background=bg)
        style.configure("Card.TFrame", background=panel)
        style.configure("TLabel", background=bg, foreground=text, font=("Segoe UI", 10))
        style.configure("Title.TLabel", background=bg, foreground="#ffffff", font=("Segoe UI Semibold", 19))
        style.configure("Hint.TLabel", background=bg, foreground="#b6c0ff", font=("Segoe UI", 10))
        style.configure("TButton", font=("Segoe UI Semibold", 10), padding=8, relief="flat", borderwidth=1)
        style.configure("Accent.TButton", font=("Segoe UI Semibold", 10), padding=8, relief="flat", borderwidth=1)
        style.map("Accent.TButton", background=[("!disabled", accent)])
        style.configure("TEntry", padding=6, relief="flat", borderwidth=1)
        style.configure("TCombobox", padding=6, relief="flat", borderwidth=1)
        style.configure(
            "TProgressbar",
            troughcolor="#262a3f",
            background=accent,
            relief="flat",
            borderwidth=0,
            bordercolor="#262a3f",
            lightcolor=accent,
            darkcolor=accent,
        )

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=18)
        outer.pack(fill=tk.BOTH, expand=True)

        ttk.Label(outer, text="Melody Machine Flasher", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            outer,
            text="Auto-detects device, lets you choose firmware, flashes ESP32-S3 with live progress and logs.",
            style="Hint.TLabel",
        ).pack(anchor="w", pady=(4, 14))

        card = ttk.Frame(outer, style="Card.TFrame", padding=14)
        card.pack(fill=tk.X)

        self.port_var = tk.StringVar()
        self.fw_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Ready")
        self.esptool_var = tk.StringVar(value="esptool: detecting...")

        row1 = ttk.Frame(card, style="Card.TFrame")
        row1.pack(fill=tk.X, pady=(0, 10))
        ttk.Label(row1, text="Device Port:", background="#171a2b").pack(side=tk.LEFT)
        self.port_combo = ttk.Combobox(row1, textvariable=self.port_var, width=50, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=10)
        ttk.Button(row1, text="Refresh", command=self.refresh_ports).pack(side=tk.LEFT)

        row2 = ttk.Frame(card, style="Card.TFrame")
        row2.pack(fill=tk.X, pady=(0, 10))
        ttk.Label(row2, text="Firmware (.bin):", background="#171a2b").pack(side=tk.LEFT)
        self.fw_entry = ttk.Entry(row2, textvariable=self.fw_var, width=72)
        self.fw_entry.pack(side=tk.LEFT, padx=10, fill=tk.X, expand=True)
        ttk.Button(row2, text="Browse", command=self.pick_firmware).pack(side=tk.LEFT)

        row3 = ttk.Frame(card, style="Card.TFrame")
        row3.pack(fill=tk.X)
        ttk.Label(row3, textvariable=self.esptool_var, background="#171a2b", foreground="#b6c0ff").pack(side=tk.LEFT)
        ttk.Label(row3, textvariable=self.status_var, background="#171a2b", foreground="#9cd6a5").pack(side=tk.RIGHT)

        progress_wrap = ttk.Frame(outer, padding=(0, 12, 0, 8))
        progress_wrap.pack(fill=tk.X)
        self.progress = ttk.Progressbar(progress_wrap, orient=tk.HORIZONTAL, mode="determinate", maximum=100)
        self.progress.pack(fill=tk.X)

        buttons = ttk.Frame(outer)
        buttons.pack(fill=tk.X, pady=(0, 10))
        self.flash_btn = ttk.Button(buttons, text="Flash Firmware", style="Accent.TButton", command=self.start_flash)
        self.flash_btn.pack(side=tk.LEFT)
        self.stop_btn = ttk.Button(buttons, text="Stop", command=self.stop_flash, state=tk.DISABLED)
        self.stop_btn.pack(side=tk.LEFT, padx=(8, 0))
        ttk.Button(buttons, text="Clear Logs", command=self.clear_logs).pack(side=tk.LEFT, padx=(8, 0))

        self.log = tk.Text(
            outer,
            height=22,
            bg="#0b0e19",
            fg="#dbe4ff",
            insertbackground="#dbe4ff",
            relief=tk.FLAT,
            borderwidth=8,
            font=("Consolas", 10),
        )
        self.log.pack(fill=tk.BOTH, expand=True)

    def _init_defaults(self) -> None:
        default_fw = find_default_firmware()
        if default_fw:
            self.fw_var.set(str(default_fw))
            self._append_log(f"[init] Default firmware: {default_fw.name}")
        else:
            self._append_log("[init] No .bin file found in project root.")

        self.refresh_ports()
        cmd, where = find_esptool_command()
        self.esptool_cmd = cmd
        self.esptool_var.set(f"esptool: {where}")
        if not cmd:
            self._append_log("[error] esptool not found. Install esptool or Arduino ESP32 tools.")

    def refresh_ports(self) -> None:
        ports = detect_ports()
        self.ports = ports
        values = [p.label for p in ports]
        self.port_combo["values"] = values
        if values:
            self.port_combo.current(0)
            self._append_log(f"[ports] Found {len(values)} port(s).")
        else:
            self.port_var.set("")
            self._append_log("[ports] No serial ports found.")

    def pick_firmware(self) -> None:
        path = filedialog.askopenfilename(
            title="Select firmware .bin",
            initialdir=str(ROOT_DIR),
            filetypes=[("Binary firmware", "*.bin"), ("All files", "*.*")],
        )
        if path:
            self.fw_var.set(path)

    def clear_logs(self) -> None:
        self.log.delete("1.0", tk.END)

    def _append_log(self, line: str) -> None:
        self.log.insert(tk.END, line.rstrip() + "\n")
        self.log.see(tk.END)

    def _poll_logs(self) -> None:
        try:
            while True:
                line = self.log_queue.get_nowait()
                self._append_log(line)
                self._update_progress_from_line(line)
        except queue.Empty:
            pass
        self.root.after(80, self._poll_logs)

    def _update_progress_from_line(self, line: str) -> None:
        m = PERCENT_RE.search(line)
        if m:
            try:
                value = float(m.group(1))
                if 0 <= value <= 100:
                    self.progress["value"] = value
            except ValueError:
                pass

    def _selected_device(self) -> str | None:
        chosen = self.port_var.get().strip()
        if not chosen:
            return None
        return chosen.split(" ", 1)[0]

    def start_flash(self) -> None:
        if self.worker and self.worker.is_alive():
            return

        if not self.esptool_cmd:
            messagebox.showerror("esptool not found", "Cannot flash because esptool was not detected.")
            return

        fw = Path(self.fw_var.get().strip())
        if not fw.exists():
            messagebox.showerror("Firmware missing", "Selected firmware file does not exist.")
            return

        port = self._selected_device()
        if not port:
            messagebox.showerror("Device not selected", "Select a serial port first.")
            return

        self.stop_event.clear()
        self.progress["value"] = 0
        self.flash_btn.configure(state=tk.DISABLED)
        self.stop_btn.configure(state=tk.NORMAL)
        self.status_var.set("Flashing...")
        self.worker = threading.Thread(target=self._flash_worker, args=(port, fw), daemon=True)
        self.worker.start()

    def stop_flash(self) -> None:
        self.stop_event.set()
        self._append_log("[user] Stop requested.")

    def _flash_worker(self, port: str, firmware: Path) -> None:
        cmd = [
            *self.esptool_cmd,
            "--chip",
            "esp32s3",
            "--port",
            port,
            "--baud",
            "921600",
            "write_flash",
            "0x10000",
            str(firmware),
        ]

        self.log_queue.put("[flash] Command:")
        self.log_queue.put("        " + " ".join(cmd))

        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True,
            )
        except Exception as exc:
            self.log_queue.put(f"[error] Failed to start esptool: {exc}")
            self.root.after(0, self._flash_finished, False)
            return

        assert proc.stdout is not None
        for line in proc.stdout:
            if self.stop_event.is_set():
                proc.terminate()
                self.log_queue.put("[flash] Process terminated by user.")
                self.root.after(0, self._flash_finished, False)
                return
            self.log_queue.put(line.rstrip())

        code = proc.wait()
        ok = code == 0 and not self.stop_event.is_set()
        self.root.after(0, self._flash_finished, ok)

    def _flash_finished(self, ok: bool) -> None:
        self.flash_btn.configure(state=tk.NORMAL)
        self.stop_btn.configure(state=tk.DISABLED)
        if ok:
            self.progress["value"] = 100
            self.status_var.set("Done")
            self._append_log("[done] Flash completed successfully.")
            messagebox.showinfo("Success", "Firmware flashed successfully.")
        else:
            self.status_var.set("Failed")
            self._append_log("[fail] Flash failed or stopped.")


def main() -> None:
    root = tk.Tk()
    app = FlasherApp(root)
    root.mainloop()
    del app


if __name__ == "__main__":
    main()
