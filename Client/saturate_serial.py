import serial
import time

# Configuration
PORT = '/dev/ttyUSB0' # Update to your NodeMCU port
BAUD = 500000
TEST_STR = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
ITERATIONS = 100

def run_stress_test():
    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
        time.sleep(2) # Wait for NodeMCU reset/init
        
        print(f"--- Starting Heavy Fire Test on {PORT} at {BAUD} ---")
        total_chars = 0
        errors = 0
        
        for i in range(ITERATIONS):
            # Send the test string
            ser.write(TEST_STR.encode())
            
            # Read the echo back
            # Note: This assumes your DUT or a loopback is returning data
            echo = ser.read(len(TEST_STR)).decode('ascii', errors='replace')
            
            for original, returned in zip(TEST_STR, echo):
                total_chars += 1
                if original != returned:
                    errors += 1
                    print(f"Error at iteration {i}: Expected '{original}', got '{returned}'")
            
            if i % 10 == 0:
                print(f"Progress: {i}/{ITERATIONS} iterations...")
        if total_chars > 0:
            success_rate = ((total_chars - errors) / total_chars) * 100
        else:
            success_rate = 0
        print("\n--- Test Results ---")
        print(f"Total Characters Sent: {total_chars}")
        print(f"Total Bit-Flips:       {errors}")
        print(f"Accuracy:              {success_rate:.4f}%")
        
        if success_rate > 99.9:
            print("Verdict: LINK STABLE (Production Grade)")
        elif success_rate > 98:
            print("Verdict: LINK USABLE (Minor Jitter)")
        else:
            print("Verdict: LINK UNSTABLE (Check Pins/Ground)")

    except Exception as e:
        print(f"Error: {e}")
    finally:
        ser.close()

if __name__ == "__main__":
    run_stress_test()