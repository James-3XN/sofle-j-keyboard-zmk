"""Send the real Windows volume to the Sofle keyboard's volume bars.

The keyboard can only guess the volume by counting knob clicks. This helper
reads the actual master volume and mute state and sends them to the keyboard
whenever they change, over USB or Bluetooth (raw HID, usage page 0xFF60).

Message (32 bytes): [0xAB, volume 0-100, mute 1=off 2=on, 0...]

Run it with pythonw.exe to keep it in the background without a window.
It only talks to the keyboard; it makes no network connections.
"""

import logging
import sys
import time

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


def run():
    device = None
    last_sent = None
    last_send_time = 0.0

    while True:
        if device is None:
            path = find_keyboard_path()
            if path is None:
                time.sleep(RECONNECT_SECONDS)
                continue
            device = hid.device()
            try:
                device.open_path(path)
            except OSError as err:
                log.warning("Found the keyboard but could not open it: %s", err)
                device = None
                time.sleep(RECONNECT_SECONDS)
                continue
            log.info("Keyboard connected")
            last_sent = None  # send the current value straight away

        try:
            current = read_volume()
        except Exception as err:  # audio device switching or briefly unavailable
            log.debug("Could not read volume: %s", err)
            time.sleep(POLL_SECONDS)
            continue

        now = time.monotonic()
        if current != last_sent or now - last_send_time >= RESEND_SECONDS:
            try:
                send(device, *current)
                last_sent, last_send_time = current, now
                log.debug("Sent volume %d%% muted=%s", *current)
            except OSError:
                log.info("Keyboard disconnected")
                device.close()
                device = None
                continue

        time.sleep(POLL_SECONDS)


def main():
    logging.basicConfig(
        level=logging.DEBUG if "--verbose" in sys.argv else logging.INFO,
        format="%(asctime)s %(message)s",
    )
    if "--list" in sys.argv:
        for info in hid.enumerate(VENDOR_ID, PRODUCT_ID):
            print("usage_page=0x%04X usage=0x%02X %s" % (info["usage_page"], info["usage"], info["path"]))
        return
    run()


if __name__ == "__main__":
    main()
