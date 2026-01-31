import serial
import time

# Use a low baud rate for the "Acid Test"
BAUD = 115200

# Open the port but DON'T initialize yet
ser = serial.Serial()
ser.port = "/dev/ttyUSB0"
ser.baudrate = BAUD
ser.timeout = 1
ser.setDTR(False)
ser.setRTS(False)

try:
    ser.open()
    # Some drivers need them to be TRUE to stay high (inactive)
    ser.dtr = True
    ser.rts = True
    time.sleep(1)

    print("Listening for data...")
    while True:
        if ser.in_waiting > 0:
            raw = ser.read(ser.in_waiting)
            print(f"Hex: {raw.hex()} | Text: {raw.decode('ascii', errors='replace')}")
        time.sleep(0.1)
except KeyboardInterrupt:
    ser.close()
