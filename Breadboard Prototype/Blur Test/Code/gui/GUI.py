"""Two-action SD media manager for the motion blur prototype."""
import datetime
from pathlib import Path
import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports
from usb_media import transfer

ROOT = Path(__file__).resolve().parents[1]


class App:
    def __init__(self, root):
        self.root = root
        root.title('Motion Blur - Media')
        root.geometry('660x260')
        root.resizable(True, True)
        root.minsize(660, 260)
        self.events = queue.Queue()
        self.stop = threading.Event()
        self.worker = None
        self.port = tk.StringVar(value='COM5')
        self.status = tk.StringVar(value="Select the board's port. Media stays on the SD card until retrieved.")
        frame = ttk.Frame(root, padding=24)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text='Motion Blur - Media', font=('Segoe UI', 18, 'bold')).pack(anchor='w')
        row = ttk.Frame(frame)
        row.pack(fill='x', pady=18)
        ttk.Label(row, text='COM port').pack(side='left')
        self.ports = ttk.Combobox(row, textvariable=self.port, width=12, postcommand=self.refresh)
        self.ports.pack(side='left', padx=(8, 20))
        self.retrieve = ttk.Button(row, text='Retrieve Media', command=lambda: self.start(False))
        self.retrieve.pack(side='left', padx=4)
        self.delete = ttk.Button(row, text='Delete Media', command=lambda: self.start(True))
        self.delete.pack(side='left', padx=4)
        self.progress = ttk.Progressbar(frame, maximum=100)
        self.progress.pack(fill='x')
        ttk.Label(frame, textvariable=self.status, wraplength=605).pack(anchor='w', pady=10)
        ttk.Label(frame, text='Downloads: retrieved_images (inside this project)').pack(anchor='w')
        self.refresh()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.poll)

    def refresh(self):
        values = [p.device for p in list_ports.comports()]
        self.ports['values'] = values
        if values and self.port.get() not in values:
            self.port.set(values[0])

    def start(self, deleting):
        if self.worker and self.worker.is_alive():
            return
        if not self.port.get().strip():
            self.status.set('Select a COM port first.')
            return
        if deleting and not messagebox.askyesno('Delete Media', "Delete all experiment JPEG and CSV files from the board's SD card?\n\nDownloaded copies on this computer will be kept."):
            return
        self.stop.clear()
        for widget in (self.retrieve, self.delete, self.ports):
            widget.configure(state='disabled')
        self.progress['value'] = 0
        self.status.set('Connecting...')
        self.worker = threading.Thread(target=self.run, args=(self.port.get().strip(), deleting), daemon=True)
        self.worker.start()

    def run(self, name, deleting):
        """Run blocking serial and USB-drive work outside Tk's UI thread."""
        output = ROOT / 'retrieved_images' / datetime.datetime.now().strftime('download_%Y%m%d_%H%M%S_%f')
        emit = lambda kind, value: self.events.put((kind, value))
        try:
            port = serial.Serial(port=None, baudrate=921600, timeout=0.2, write_timeout=2)
            port.dtr = False
            port.rts = False
            port.port = name
            with port:
                transfer(port, deleting, output, self.stop, emit)
        except serial.SerialTimeoutException:
            emit('status', 'The board is not accepting commands. Reset it, wait for startup, and retry.')
        except (RuntimeError, ValueError, OSError, serial.SerialException) as exc:
            emit('status', str(exc))
        finally:
            emit('idle', None)

    def poll(self):
        for _ in range(300):
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == 'status':
                self.status.set(value)
            elif kind == 'progress':
                _, received, total = value
                self.progress['value'] = received*100/total if total else 0
            elif kind == 'idle':
                for widget in (self.retrieve, self.delete, self.ports):
                    widget.configure(state='normal')
        self.root.after(100, self.poll)

    def close(self):
        self.stop.set()
        if self.worker and self.worker.is_alive():
            self.root.after(100, self.close)
        else:
            self.root.destroy()


if __name__ == '__main__':
    root = tk.Tk()
    App(root)
    root.mainloop()
