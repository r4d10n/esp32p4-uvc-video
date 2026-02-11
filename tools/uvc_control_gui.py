#!/usr/bin/env python3
"""
ESP32-P4 UVC Control Panel

Tkinter GUI for controlling UVC Processing Unit parameters and
ISP Color Profile via Extension Unit on the ESP32-P4 webcam.

PU controls use v4l2-ctl (subprocess).
XU ISP profile uses pyusb raw USB control transfers — the Linux
UVCIOC_CTRL_MAP ioctl is broken on kernel 6.5.x for this device.

Requirements:
    - Python 3.7+
    - tkinter (stdlib)
    - v4l2-ctl (v4l-utils package)
    - pyusb   (pip install pyusb)
    - Linux with uvcvideo driver

Usage:
    python3 tools/uvc_control_gui.py
"""

import subprocess
import glob
import re
import time
import tkinter as tk
from contextlib import contextmanager
from tkinter import ttk

# Optional pyusb for XU controls
try:
    import usb.core
    import usb.util
    HAS_PYUSB = True
except ImportError:
    HAS_PYUSB = False

# ---------- USB constants ----------

ESP32_VID = 0x303a
ESP32_PID = 0x8000

# UVC class-specific request codes
UVC_GET_CUR = 0x81
UVC_SET_CUR = 0x01

# XU entity/selector — must match firmware usb_descriptors.h
XU_UNIT_ID = 0x04
XU_SELECTOR = 0x01
XU_WVALUE = XU_SELECTOR << 8           # 0x0100
XU_WINDEX = (XU_UNIT_ID << 8) | 0x00   # 0x0400  (interface 0)


# ---------- pyusb XU helpers ----------

@contextmanager
def _xu_access(dev):
    """Detach kernel driver from VC interface for raw USB access, reattach on exit."""
    detached = False
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
            detached = True
        yield dev
    finally:
        if detached:
            try:
                usb.util.release_interface(dev, 0)
            except Exception:
                pass
            try:
                dev.attach_kernel_driver(0)
            except Exception:
                pass
            # Brief pause for kernel to re-bind uvcvideo
            time.sleep(0.3)


def xu_find_device():
    """Find the ESP32-P4 USB device. Returns usb.core.Device or None."""
    if not HAS_PYUSB:
        return None
    return usb.core.find(idVendor=ESP32_VID, idProduct=ESP32_PID)


def xu_get_cur(dev):
    """GET_CUR on XU ISP Profile control. Returns profile index or None."""
    try:
        with _xu_access(dev):
            ret = dev.ctrl_transfer(0xA1, UVC_GET_CUR, XU_WVALUE, XU_WINDEX,
                                    1, timeout=2000)
            return ret[0]
    except Exception:
        return None


def xu_set_cur(dev, value):
    """SET_CUR on XU ISP Profile control. Returns True on success."""
    try:
        with _xu_access(dev):
            dev.ctrl_transfer(0x21, UVC_SET_CUR, XU_WVALUE, XU_WINDEX,
                              bytes([value & 0xFF]), timeout=2000)
            return True
    except Exception:
        return False


# ---------- v4l2-ctl helpers ----------

def v4l2_get_ctrl(device, ctrl_name):
    """Get a V4L2 control value via v4l2-ctl."""
    try:
        result = subprocess.run(
            ['v4l2-ctl', '-d', device, '--get-ctrl', ctrl_name],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode == 0:
            m = re.search(r':\s*(-?\d+)', result.stdout)
            if m:
                return int(m.group(1))
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    return None


def v4l2_set_ctrl(device, ctrl_name, value):
    """Set a V4L2 control value via v4l2-ctl."""
    try:
        subprocess.run(
            ['v4l2-ctl', '-d', device, '--set-ctrl', f'{ctrl_name}={value}'],
            capture_output=True, text=True, timeout=5,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass


def find_esp32p4_devices():
    """Scan /dev/video* for ESP32-P4 UVC devices."""
    devices = []
    for dev in sorted(glob.glob('/dev/video*')):
        try:
            result = subprocess.run(
                ['v4l2-ctl', '-d', dev, '--info'],
                capture_output=True, text=True, timeout=3,
            )
            if result.returncode == 0 and 'ESP' in result.stdout:
                m = re.search(r'Card\s+type\s*:\s*(.+)', result.stdout)
                card = m.group(1).strip() if m else dev
                devices.append((dev, card))
        except (subprocess.TimeoutExpired, FileNotFoundError):
            continue
    return devices


# ---------- ISP Profile names ----------

ISP_PROFILES = [
    (0, 'Tungsten (2873K)'),
    (1, 'Indoor-Warm (3725K)'),
    (2, 'Fluorescent (5095K)'),
    (3, 'Daylight (6015K)'),
    (4, 'Cloudy (6865K)'),
    (5, 'Shade (7600K)'),
]

# ---------- PU control definitions ----------

PU_CONTROLS = [
    # (v4l2_name, label, min, max, default)
    ('brightness',  'Brightness',  -127, 127, 0),
    ('contrast',    'Contrast',    0,    256, 128),
    ('hue',         'Hue',         0,    255, 0),
    ('saturation',  'Saturation',  0,    256, 128),
]


# ---------- GUI ----------

class UVCControlPanel:
    def __init__(self, root):
        self.root = root
        self.root.title('ESP32-P4 UVC Control Panel')
        self.root.resizable(True, True)
        self.root.minsize(420, 500)

        self.device = tk.StringVar()
        self.profile_var = tk.IntVar(value=3)
        self.usb_dev = None  # pyusb device handle
        self.xu_available = False
        self.sliders = {}

        self._build_ui()
        self._refresh_devices()

    def _build_ui(self):
        pad = {'padx': 8, 'pady': 4}

        # ---- Device selector ----
        dev_frame = ttk.LabelFrame(self.root, text='Device', padding=8)
        dev_frame.pack(fill='x', **pad)

        self.device_combo = ttk.Combobox(dev_frame, textvariable=self.device,
                                         state='readonly', width=30)
        self.device_combo.pack(side='left', fill='x', expand=True)
        self.device_combo.bind('<<ComboboxSelected>>', self._on_device_change)

        ttk.Button(dev_frame, text='Refresh', command=self._refresh_devices).pack(
            side='left', padx=(8, 0))

        # ---- PU Controls ----
        pu_frame = ttk.LabelFrame(self.root, text='Processing Unit Controls', padding=8)
        pu_frame.pack(fill='x', **pad)

        for ctrl_name, label, cmin, cmax, cdef in PU_CONTROLS:
            row = ttk.Frame(pu_frame)
            row.pack(fill='x', pady=2)

            ttk.Label(row, text=label, width=12, anchor='w').pack(side='left')

            var = tk.IntVar(value=cdef)
            val_label = ttk.Label(row, text=str(cdef), width=5, anchor='e')
            val_label.pack(side='right')

            slider = ttk.Scale(
                row, from_=cmin, to=cmax, variable=var, orient='horizontal',
                command=lambda v, cn=ctrl_name, vr=var, vl=val_label:
                    self._on_slider(cn, vr, vl),
            )
            slider.pack(side='left', fill='x', expand=True, padx=(4, 4))

            self.sliders[ctrl_name] = (var, val_label, cmin, cmax, cdef)

        # ---- ISP Color Profile ----
        isp_frame = ttk.LabelFrame(self.root, text='ISP Color Profile', padding=8)
        isp_frame.pack(fill='x', **pad)

        self.xu_status = ttk.Label(isp_frame, text='', foreground='gray')
        self.xu_status.pack(anchor='w')

        self.profile_buttons = []
        for idx, name in ISP_PROFILES:
            rb = ttk.Radiobutton(
                isp_frame, text=name, variable=self.profile_var, value=idx,
                command=self._on_profile_change,
            )
            rb.pack(anchor='w', pady=1)
            self.profile_buttons.append(rb)

        # ---- Reset button ----
        btn_frame = ttk.Frame(self.root, padding=8)
        btn_frame.pack(fill='x')

        ttk.Button(btn_frame, text='Reset All to Defaults',
                   command=self._reset_all).pack(fill='x')

        # ---- Status bar ----
        self.status = ttk.Label(self.root, text='Ready', relief='sunken',
                                anchor='w', padding=(4, 2))
        self.status.pack(fill='x', side='bottom')

    def _refresh_devices(self):
        self._set_status('Scanning for devices...')
        devices = find_esp32p4_devices()
        self._devices = devices

        # Also try to find the USB device for XU
        self.usb_dev = xu_find_device()

        if devices:
            self.device_combo['values'] = [f'{d} -- {c}' for d, c in devices]
            self.device_combo.current(0)
            self.device.set(self.device_combo['values'][0])
            self._on_device_change()
        else:
            self.device_combo['values'] = ['No ESP32-P4 device found']
            self.device_combo.current(0)
            self._set_status('No device found. Is the webcam connected?')

    def _get_device_path(self):
        if not self._devices:
            return None
        idx = self.device_combo.current()
        if 0 <= idx < len(self._devices):
            return self._devices[idx][0]
        return None

    def _on_device_change(self, event=None):
        dev = self._get_device_path()
        if not dev:
            return

        self._set_status(f'Connected to {dev}')

        # Read current PU control values
        for ctrl_name, _, _, _, cdef in PU_CONTROLS:
            val = v4l2_get_ctrl(dev, ctrl_name)
            if val is not None:
                var, val_label, _, _, _ = self.sliders[ctrl_name]
                var.set(val)
                val_label.config(text=str(val))

        # Probe XU availability
        if self.usb_dev is not None:
            cur = xu_get_cur(self.usb_dev)
            if cur is not None:
                self.xu_available = True
                self.profile_var.set(cur)
                self.xu_status.config(
                    text=f'XU connected via pyusb (current: {cur})',
                    foreground='green')
                for rb in self.profile_buttons:
                    rb.configure(state='normal')
            else:
                self._xu_unavailable('XU read failed (permission? try sudo)')
        elif not HAS_PYUSB:
            self._xu_unavailable('pyusb not installed (pip install pyusb)')
        else:
            self._xu_unavailable('ESP32-P4 USB device not found')

    def _xu_unavailable(self, msg):
        self.xu_available = False
        self.xu_status.config(text=msg, foreground='red')
        for rb in self.profile_buttons:
            rb.configure(state='disabled')

    def _on_slider(self, ctrl_name, var, val_label):
        value = int(float(var.get()))
        var.set(value)
        val_label.config(text=str(value))

        dev = self._get_device_path()
        if dev:
            v4l2_set_ctrl(dev, ctrl_name, value)

    def _on_profile_change(self):
        if not self.xu_available or self.usb_dev is None:
            self._set_status('Cannot set profile: XU not available')
            return

        idx = self.profile_var.get()
        self._set_status(f'Setting ISP profile {idx}...')
        self.root.update_idletasks()

        ok = xu_set_cur(self.usb_dev, idx)
        if ok:
            self._set_status(f'ISP profile: {ISP_PROFILES[idx][1]}')
        else:
            self._set_status(f'Failed to set ISP profile {idx}')

    def _reset_all(self):
        dev = self._get_device_path()
        if not dev:
            return

        for ctrl_name, _, _, _, cdef in PU_CONTROLS:
            var, val_label, _, _, _ = self.sliders[ctrl_name]
            var.set(cdef)
            val_label.config(text=str(cdef))
            v4l2_set_ctrl(dev, ctrl_name, cdef)

        self.profile_var.set(3)  # Daylight default
        if self.xu_available and self.usb_dev is not None:
            xu_set_cur(self.usb_dev, 3)

        self._set_status('All controls reset to defaults')

    def _set_status(self, text):
        self.status.config(text=text)
        self.root.update_idletasks()


def main():
    # Check for v4l2-ctl
    try:
        subprocess.run(['v4l2-ctl', '--version'], capture_output=True, timeout=3)
    except FileNotFoundError:
        print('Error: v4l2-ctl not found. Install v4l-utils:')
        print('  sudo pacman -S v4l-utils    # Arch/Manjaro')
        print('  sudo apt install v4l-utils   # Debian/Ubuntu')
        return 1

    if not HAS_PYUSB:
        print('Warning: pyusb not installed. ISP profile control will be disabled.')
        print('  pip install pyusb')

    root = tk.Tk()
    UVCControlPanel(root)
    root.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
