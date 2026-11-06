#!/usr/bin/env python3
"""Saves terrarium photos arriving over USB serial as .jpg files.

Plug in the Heltec receiver (or the ESP32-CAM on its MB board, for testing) and run:

    python3 save_images.py /dev/ttyUSB0            # receiver: saves every photo it gets
    python3 save_images.py /dev/ttyUSB0 --snap     # ESP32-CAM: asks for one photo and saves it

Every other line from the board is printed as-is, so this also works as a serial monitor.
Photos arrive as lines of the form:  IMAGE <id> <length> <crc32 hex> <base64 JPEG>
"""
import argparse
import base64
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

import serial


def save(line, out_dir):
    try:
        _, image_id, length, crc, data = line.split(" ", 4)
        jpeg = base64.b64decode(data, validate=True)
    except ValueError:
        print("!! couldn't parse an IMAGE line, skipping it")
        return
    if len(jpeg) != int(length) or zlib.crc32(jpeg) != int(crc, 16):
        print(f"!! photo {image_id} is damaged (length or checksum mismatch), not saved")
        return
    path = out_dir / f"terrarium_{datetime.now():%Y%m%d_%H%M%S}_{image_id}.jpg"
    path.write_bytes(jpeg)
    print(f">> saved photo {image_id}: {path} ({len(jpeg)} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="serial port, e.g. /dev/ttyUSB0 or COM3")
    parser.add_argument("--out", default="images", help="folder to save photos in (default: images)")
    parser.add_argument("--snap", action="store_true", help="ask an ESP32-CAM for one photo, save it, then exit")
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 1
    port.dtr = port.rts = False  # set before opening, or the ESP32-CAM-MB holds the board in reset
    with port:
        if args.snap:
            time.sleep(2)  # let the CAM finish booting if opening the port reset it
            port.reset_input_buffer()
            port.write(b"SNAPB64\n")
        deadline = time.time() + 20
        while True:
            raw = port.readline()
            if not raw:
                if args.snap and time.time() > deadline:
                    sys.exit("No photo received from the camera within 20 s")
                continue
            line = raw.decode(errors="replace").strip()
            if line.startswith("IMAGE "):
                save(line, out_dir)
                if args.snap:
                    return
            elif line:
                print(line)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
