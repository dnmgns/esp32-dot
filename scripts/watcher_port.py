"""Find the Watcher's ESP32-S3 serial port.

The SenseCAP Watcher's USB-C goes to a dual USB-to-serial bridge (reported as
WCH CH342, 1a86:55d2). Seeed's wiki says the second port ("B", the higher
device name) is the ESP32-S3 and the first is the Himax AI chip.
Set PIP_PORT to override detection.
"""
import os

from serial.tools import list_ports

BRIDGE_IDS = {(0x1A86, 0x55D2)}


def candidates():
    forced = os.environ.get("PIP_PORT")
    if forced:
        return [forced]
    bridge = sorted(p.device for p in list_ports.comports() if (p.vid, p.pid) in BRIDGE_IDS)
    return bridge[-1:]
