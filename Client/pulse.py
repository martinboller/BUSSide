import serial
import time

# 1. Match the speed exactly
BAUD = 115200 
ser = serial.Serial('/dev/ttyUSB0', BAUD, timeout=1, dsrdtr=False, rtscts=False)

# 2. Wait for the 'Welcome' garbage to finish
print("Waiting for board to stabilize...")
time.sleep(2)

# 3. Drain the buffer (This removes the '44' and other noise)
ser.reset_input_buffer()

print("Sending Sync...")
ser.write(b'\xfe\xca')

# 4. Read the reply
response = ser.read(1)
if response:
    print(f"SUCCESS: Received {response.hex()} ({response})")
else:
    print("FAILED: Received nothing")

ser.close()