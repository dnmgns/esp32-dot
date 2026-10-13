"""Run esptool with serial writes cut into small, paced chunks.

On the SenseCAP Watcher's USB-serial bridge, bulk host-to-chip writes lose or
corrupt bytes: the stub upload fails with a checksum error and ROM flash writes
fail with 0x0105/0x0107. Chunks of 16 bytes with a flush and a short gap work;
64 bytes does not. Usage: esptool_paced.py <chunk> <esptool arguments...>
"""
import sys
import time

import serial.serialposix as serialposix

CHUNK = int(sys.argv.pop(1))
_write = serialposix.Serial.write


def write(self, data):
    data = bytes(data)
    written = 0
    for offset in range(0, len(data), CHUNK):
        written += _write(self, data[offset:offset + CHUNK])
        self.flush()
        time.sleep(0.002)
    return written


serialposix.Serial.write = write

import esptool  # noqa: E402

esptool._main()
