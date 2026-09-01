#!/usr/bin/env python3
"""Read-only serial capture for Clare S3 boot verification.
Usage: python3 capture_serial.py <port> <seconds> <outfile>
Does not send any data; toggles DTR/RTS once to reset USB-Serial/JTAG.
"""
import sys, time
import serial

port, seconds, outfile = sys.argv[1], float(sys.argv[2]), sys.argv[3]

with serial.Serial() as s:
    s.port = port
    s.baudrate = 115200
    s.dtr = False
    s.rts = True
    s.timeout = 0.2
    s.open()
    # reset pulse for USB-Serial/JTAG
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    s.dtr = False
    t0 = time.time()
    with open(outfile, "wb") as f:
        while time.time() - t0 < seconds:
            try:
                data = s.read(4096)
            except (OSError, serial.SerialException):
                # USB-Serial/JTAG briefly drops during reset; wait and retry
                time.sleep(0.5)
                continue
            if data:
                f.write(data)
                f.flush()
print(f"captured {seconds}s -> {outfile}")
