"""Send the real Windows volume to the Sofle keyboard's volume bars.

The keyboard can only guess the volume by counting knob clicks. This helper
reads the actual master volume and mute state and sends them to the keyboard
whenever they change, over USB or Bluetooth (raw HID, usage page 0xFF60).

Message (32 bytes): [0xAB, volume 0-100, mute 1=off 2=on, 0...]

It sits in the system tray (next to the clock). The tray menu shows whether
the keyboard is connected, can start the helper with Windows, opens the log
and quits. Without a console (the .exe, or pythonw.exe) it logs to
%LOCALAPPDATA%\\SofleVolumeHelper\\helper.log.

It only talks to the keyboard; it makes no network connections. "Start with
Windows" adds one entry under HKEY_CURRENT_USER\\...\\Run (this user only,
no admin rights needed); unticking it removes the entry again.

Options: --no-tray (run in the console only), --verbose, --list, --selftest
"""

import logging
import os
import sys
import threading
import time
import winreg

import hid
from pycaw.pycaw import AudioUtilities

VENDOR_ID = 0x1D50   # ZMK default
PRODUCT_ID = 0x615E  # ZMK default (USB and Bluetooth)
USAGE_PAGE = 0xFF60  # raw HID interface added by zmk-raw-hid
USAGE = 0x61

VOLUME_MESSAGE = 0xAB
REPORT_SIZE = 32

POLL_SECONDS = 0.25      # how often the local volume is read (no traffic unless it changed)
RESEND_SECONDS = 60      # resend unchanged value now and then, e.g. after a keyboard restart
RECONNECT_SECONDS = 3    # how often to look for the keyboard while it's not connected

APP_NAME = "SofleVolumeHelper"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"

log = logging.getLogger("volume_helper")


def find_keyboard_path():
    for info in hid.enumerate(VENDOR_ID, PRODUCT_ID):
        if info["usage_page"] == USAGE_PAGE and info["usage"] == USAGE:
            return info["path"]
    return None


def read_volume():
    """Return (percent, muted) for the default playback device."""
    volume = AudioUtilities.GetSpeakers().EndpointVolume
    percent = round(volume.GetMasterVolumeLevelScalar() * 100)
    return percent, bool(volume.GetMute())


def send(device, percent, muted):
    report = bytes([VOLUME_MESSAGE, percent, 2 if muted else 1]).ljust(REPORT_SIZE, b"\0")
    # hidapi on Windows wants the report id (0) in front
    if device.write(b"\0" + report) < 0:
        raise OSError("write failed")


class Status:
    """What the tray shows; updated by the worker thread."""

    def __init__(self):
        self.connected = False
        self.volume = None
        self.on_change = lambda: None

    def set(self, connected, volume=None):
        if (connected, volume) != (self.connected, self.volume):
            self.connected, self.volume = connected, volume
            self.on_change()

    def text(self):
        if not self.connected:
            return "Waiting for the keyboard"
        if self.volume is None:
            return "Keyboard connected"
        percent, muted = self.volume
        return "Keyboard connected - %s" % ("muted" if muted else "%d%%" % percent)


def run(status, stop):
    # COM (used by pycaw) must be initialised in each thread that uses it
    import comtypes

    comtypes.CoInitialize()
    device = None
    last_sent = None
    last_send_time = 0.0

    try:
        while not stop.is_set():
            if device is None:
                path = find_keyboard_path()
                if path is None:
                    status.set(False)
                    stop.wait(RECONNECT_SECONDS)
                    continue
                device = hid.device()
                try:
                    device.open_path(path)
                except OSError as err:
                    log.warning("Found the keyboard but could not open it: %s", err)
                    device = None
                    stop.wait(RECONNECT_SECONDS)
                    continue
                log.info("Keyboard connected")
                status.set(True)
                last_sent = None  # send the current value straight away

            try:
                current = read_volume()
            except Exception as err:  # audio device switching or briefly unavailable
                log.debug("Could not read volume: %s", err)
                stop.wait(POLL_SECONDS)
                continue

            now = time.monotonic()
            if current != last_sent or now - last_send_time >= RESEND_SECONDS:
                try:
                    send(device, *current)
                    last_sent, last_send_time = current, now
                    status.set(True, current)
                    log.debug("Sent volume %d%% muted=%s", *current)
                except OSError:
                    log.info("Keyboard disconnected")
                    device.close()
                    device = None
                    status.set(False)
                    continue

            stop.wait(POLL_SECONDS)
    finally:
        if device is not None:
            device.close()
        comtypes.CoUninitialize()


# --- Start with Windows (HKCU Run entry) ---

def startup_command():
    if getattr(sys, "frozen", False):
        return '"%s"' % sys.executable
    # Running from source: use pythonw.exe so no console window opens at login
    pythonw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    return '"%s" "%s"' % (pythonw, os.path.abspath(__file__))


def startup_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.QueryValueEx(key, APP_NAME)
            return True
    except OSError:
        return False


def set_startup(enabled):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
        if enabled:
            winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, startup_command())
        else:
            try:
                winreg.DeleteValue(key, APP_NAME)
            except FileNotFoundError:
                pass
    log.info("Start with Windows: %s", "on" if enabled else "off")


# --- Tray icon ---

def make_icon(connected):
    """Four rising volume bars; white when connected, grey while waiting."""
    from PIL import Image, ImageDraw

    image = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    fill = (255, 255, 255, 255) if connected else (140, 140, 140, 255)
    for i in range(4):
        height = 16 + i * 14
        x = 4 + i * 15
        draw.rectangle([x, 62 - height, x + 11, 62], fill=fill, outline=(0, 0, 0, 255), width=2)
    return image


def run_tray(status, stop):
    import pystray

    icons = {True: make_icon(True), False: make_icon(False)}

    def on_quit(icon, _item):
        stop.set()
        icon.stop()

    def on_toggle_startup(_icon, _item):
        set_startup(not startup_enabled())

    def on_open_log(_icon, _item):
        if os.path.exists(log_file_path()):
            os.startfile(log_file_path())

    icon = pystray.Icon(
        APP_NAME,
        icons[False],
        "Sofle volume - " + status.text(),
        menu=pystray.Menu(
            pystray.MenuItem(lambda _item: status.text(), None, enabled=False),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Start with Windows", on_toggle_startup,
                             checked=lambda _item: startup_enabled()),
            pystray.MenuItem("Open log", on_open_log),
            pystray.MenuItem("Quit", on_quit),
        ),
    )

    def refresh():
        icon.icon = icons[status.connected]
        icon.title = "Sofle volume - " + status.text()
        icon.update_menu()

    status.on_change = refresh
    icon.run()  # blocks until Quit


# --- Startup ---

def log_file_path():
    folder = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), APP_NAME)
    os.makedirs(folder, exist_ok=True)
    return os.path.join(folder, "helper.log")


def already_running():
    """True if another copy holds the named mutex (e.g. the .exe was double-clicked twice)."""
    import ctypes

    ERROR_ALREADY_EXISTS = 183
    kernel32 = ctypes.windll.kernel32
    # Keep a reference so the mutex lives as long as the process
    main.mutex = kernel32.CreateMutexW(None, False, "Local\\" + APP_NAME)
    return kernel32.GetLastError() == ERROR_ALREADY_EXISTS


def main():
    one_off = "--list" in sys.argv or "--selftest" in sys.argv
    # Check before logging is set up, so a second copy doesn't wipe the first one's log
    if not one_off and already_running():
        return

    # Without a console (the .exe, pythonw) log to a small file instead (overwritten each start).
    has_console = sys.stdout is not None
    logging.basicConfig(
        level=logging.DEBUG if "--verbose" in sys.argv else logging.INFO,
        format="%(asctime)s %(message)s",
        **({} if has_console else {"filename": log_file_path(), "filemode": "w"}),
    )
    if "--list" in sys.argv:
        for info in hid.enumerate(VENDOR_ID, PRODUCT_ID):
            log.info("usage_page=0x%04X usage=0x%02X %s", info["usage_page"], info["usage"], info["path"])
        return
    if "--selftest" in sys.argv:
        log.info("Volume now: %d%% muted=%s", *read_volume())
        log.info("Keyboard raw HID found: %s", find_keyboard_path() is not None)
        log.info("Start with Windows: %s", startup_enabled())
        return

    log.info("Started")
    status = Status()
    stop = threading.Event()

    if "--no-tray" in sys.argv:
        try:
            run(status, stop)
        except KeyboardInterrupt:
            pass
        return

    worker = threading.Thread(target=run, args=(status, stop), daemon=True)
    worker.start()
    run_tray(status, stop)
    worker.join(timeout=2)
    log.info("Quit")


if __name__ == "__main__":
    main()
