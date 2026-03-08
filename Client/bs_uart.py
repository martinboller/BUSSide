#!/usr/bin/env python3

"""Minimal UART helpers for BUSSide client.

This file contains small utility functions that call into the low-level
`bs` framing API to discover UART signals and to provide a passthrough
terminal mode.
"""

import bs
import time
import sys
import struct

def uart_data_discover():
    """Request the device to sample GPIO activity and report change counts.

    Returns `(length, args)` on success or `None` on failure.
    """
    print("+++ Sending UART data discovery command")
    request_args = []
    bs.NewTimeout(60)
    rv = bs.requestreply(15, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv

    ngpio = 9
    for i in range(ngpio):
        #print("+++ SIGNAL CHANGES: D%d --> %d" % ((i + 1), bs_reply_args[i]))
        if bs_reply_args[i] != 0:
            print("+++-----------------------------------------------+++")
            print("+++ SIGNAL CHANGES: D%d --> %d" % ((i + 1), bs_reply_args[i]))
            print("+++-----------------------------------------------+++")
        else:
            print("+++ GPIO %d: No signal changes detected" % (i + 1))
    
    print("+++ Note: GPIO 1 may show activity but can't be used for UART.")
    print("+++")        
    print("+++ UART Data Discover Command Successfully Completed\n")
    return rv


def uart_tx(rxpin, baudrate):
    print("+++ Sending UART discovery tx command")
    request_args = [rxpin, baudrate]
    bs.NewTimeout(3)
    rv = bs.requestreply(21, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv

    txpin = bs_reply_args[0]
    if txpin != 0xFFFFFFFF:
        print("+++ FOUND UART TX on GPIO %d" % (txpin + 1))
    else:
        print("+++ NOT FOUND. Note that GPIO 1 can't be used.")
    print("+++ UART Discovery TX Command Successfully Completed\n")
    return rv

def uart_rx():
    print("+++ Sending UART discovery rx command")
    request_args = []
    bs.NewTimeout(120)
    rv = bs.requestreply(11, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv

    ngpio = 9
    # Safety check: ensure we have enough data for all GPIOs
    # Each GPIO needs 5 slots. Total needed = 45.
    expected_len = ngpio * 5

    for i in range(ngpio):
        base = 5 * i
        # Verify the base index exists in the returned data
        if len(bs_reply_args) > base:
            changes = bs_reply_args[base + 0]
            print("+++ GPIO %d has %d signal changes" % (i + 1, changes))
            
            if changes > 0 and len(bs_reply_args) >= (base + 5):
                databits = bs_reply_args[base + 1]
                if databits > 0:
                    stopbits = bs_reply_args[base + 2]
                    parity = bs_reply_args[base + 3]
                    baudrate = bs_reply_args[base + 4]
                    
                    print("+++ UART FOUND")
                    print("+++ BAUDRATE: %d" % (baudrate))
                    print("+++ DATABITS: %d" % (databits))
                    
                    if parity == 0:
                        print("+++ PARITY: EVEN")
                    elif parity == 1:
                        print("+++ PARITY: ODD")
                    else:
                        print("+++ PARITY: NONE")
                    print("+++ STOPBITS: %d" % (stopbits))
        else:
            print("--- GPIO %d: No data received from hardware" % (i + 1))
    print("+++ UART Discovery RX Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)

def uart_get_status():
    """Queries the hardware for the current persistent UART settings."""
    print("+++ Querying Hardware UART Status...")
    bs.NewTimeout(2)
    # Command 48: BS_UART_GET_CONFIG
    rv = bs.requestreply(48, [])
    
    if rv is None:
        print("--- Error: No response from hardware.")
        return None

    _, args = rv
    if len(args) < 3:
        print("--- Error: Malformed status response.")
        return None

    dbits = args[0]
    sbits = args[1]
    p_val = args[2]

    # Map the integer back to a string for the user
    # Note: C++ uses -1 (represented as 4294967295 in unsigned 32-bit) for None
    p_name = "NONE"
    if p_val == 0: p_name = "EVEN"
    elif p_val == 1: p_name = "ODD"
    elif p_val > 2: p_name = "NONE" # Catch-all for -1 unsigned wrap

    print("+++-----------------------------------+++")
    print(f"+++ CURRENT HW CONFIG: {dbits}{p_name[0]}{sbits}")
    print(f"+++ Data Bits: {dbits}")
    print(f"+++ Stop Bits: {sbits}")
    print(f"+++ Parity:    {p_name}")
    print("+++-----------------------------------+++")
    return True

def uart_config(dbits, parity_char, sbits):
    """Sets the global UART parameters on the BUSSide hardware."""
    try:
        # Convert parity character to the numeric index the C++ code expects
        # -1: None (0), 0: Even (1), 1: Odd (2)
        p_map = {"n": 0, "e": 1, "o": 2}
        p_val = p_map.get(str(parity_char).lower(), 0)
        
        # Pack into mask: [Parity(bits 8-11)][Stop(bits 4-7)][Data(bits 0-3)]
        mask = (int(dbits) & 0x0F)
        mask |= (int(sbits) & 0x0F) << 4
        mask |= (p_val & 0x0F) << 8
        
        print(f"+++ Sending Config (ID 47) | Mask: {hex(mask)} ({dbits}{parity_char.upper()}{sbits})")
        
        bs.NewTimeout(2)
        # Use 47 to match #define BS_UART_SET_CONFIG 47 in busside.h
        return bs.requestreply(47, [mask])
    except Exception as e:
        print(f"--- UART Config Error: {e}")
        return None
    
def uart_passthrough(gpiorx, gpiotx, baudrate):
    # Convert indices (1-based to 0-based)
    rx_idx = int(gpiorx) - 1
    tx_idx = int(gpiotx) - 1
    if tx_idx < 0 or tx_idx > 250: tx_idx = 255
    
    # Use bs serial port directly.
    ser = bs.getSerial()
    
    print(f"+++ Forcing Bridge Entry: RX={gpiorx}, TX={gpiotx}, @{baudrate}")
    
    # Internal request-reply but force it to stop after one try
    # by setting a very short timeout and catching the error.
    bs.NewTimeout(1)
    try:
        bs.requestreply(19, [rx_idx, tx_idx, int(baudrate)])
    except:
        pass # Ignore the timeout/sync error
    
    # Breathe and stabilize
    time.sleep(0.5)
    ser.reset_input_buffer()
    
    # Terminal Loop
    bs.keys_init()
    print("+++ Terminal Started (Press CTRL+X then Ctrl+C to exit)")
    try:
        while True:
            if ser.in_waiting > 0:
                raw = ser.read(ser.in_waiting)
                sys.stdout.write(raw.decode("latin-1", errors="replace"))
                sys.stdout.flush()
                # Add a small buffer to check for the exit string
                line_buffer = ""
                for char in raw.decode("latin-1", errors="replace"):
                    line_buffer += char
                    if "BUSSIDE_EXIT_UART_PASSTHROUGH" in line_buffer:
                        print("\n[!] Device signaled exit. Returning to BUSSide...")
                        return

            char = bs.keys_getchar()
            if char is not None:
                ser.write(char.encode("utf-8"))
            
            time.sleep(0.01)
    except KeyboardInterrupt:
        print("\n+++ Terminating...")
    finally:
        bs.keys_cleanup()
        ser.write(b'\xfe\xca')

def terminal_loop_robust(ser):
    bs.keys_init()
    print("--- TERMINAL ACTIVE ---")
    try:
        while True:
            if ser.in_waiting > 0:
                data = ser.read(ser.in_waiting)
                # Try decoding with 'latin-1'—it never fails/crashes like utf-8
                sys.stdout.write(data.decode("latin-1", errors="replace"))
                sys.stdout.flush()
            
            char = bs.keys_getchar()
            if char is not None:
                ser.write(char.encode("utf-8"))
            time.sleep(0.01)
    except KeyboardInterrupt:
        print("\n+++ Exiting...")
    finally:
        bs.keys_cleanup()
        ser.write(b'\xfe\xca')

def uart_passthrough_auto():
    txpin = 0xFFFFFFFF
    # 1. Discover active RX lines
    rv = uart_rx()
    print("+++ Starting UART Auto-Discovery")
    
    if rv is None:
        print("+++ NOT FOUND")
        return 0
        
    bs_reply_length, bs_reply_args = rv
    found_candidates = []
    ngpio = 9
    
    # 2. Identify candidate pins (using 0-based indices)
    for i in range(ngpio):
        changes = bs_reply_args[5 * i + 0]
        if changes > 50: # Threshold to ignore noise
            baudrate = bs_reply_args[5 * i + 4]
            # Use raw index 'i' (do NOT add 1)
            found_candidates.append({'rx': i, 'baud': baudrate})
            
    if not found_candidates:
        print("+++ NO ACTIVE UART SIGNALS DETECTED")
        return 0
        
    if len(found_candidates) > 1:
        print(f"+++ Found {len(found_candidates)} active lines. Picking the first one...")
    
    # Select our best candidate
    target = found_candidates[0]
    rxpin = target['rx']
    baudrate = target['baud']
    
    print(f"+++ Detected RX on Index {rxpin} at {baudrate} baud.")
    print("+++ Waiting for line to idle before TX discovery...")
    time.sleep(5)

    # 3. Attempt to find the TX pin (the pin we talk TO)
    for j in range(3):
        rv = uart_tx(rxpin, baudrate)
        if rv is not None:
            tx_reply_args = rv
            detected_tx = tx_reply_args[0]
            if detected_tx != 0xFFFFFFFF:
                txpin = detected_tx # Use raw index
                print(f"+++ Found corresponding TX on Index {txpin}")
                break
        print(f"+++ TX Detection attempt {j+1} failed. Retrying...")
        time.sleep(2)

    if txpin == rxpin or txpin == 0xFFFFFFFF:
        print(f"+++ TX pin ({txpin}) is invalid or matches RX ({rxpin}).")
        print("+++ Defaulting to Sniff-Only mode (TX disabled).")
        txpin = 255 # Standard 'No Pin' value for the firmware
        
    # 4. Trigger Passthrough
    return uart_passthrough(rxpin, txpin, baudrate)

def doCommand(command):
    if command == "discover rx":
        uart_rx()
        return 0
    elif command == "discover data":
        uart_data_discover()
        return 0
    elif command.find("discover tx ") == 0:
        args = command[12:].split()
        if len(args) != 2:
            return None
        uart_tx(int(args[0]), int(args[1]))
        return 0
    elif command == "passthrough auto":
        uart_passthrough_auto()
        return 0
    elif command.startswith("passthrough "):
        args = command.split()
        if len(args) != 4: # passthrough RX TX BAUD DATABITS STOPBITS PARITY
            print("Usage: passthrough <rx_pin> <tx_pin> <baud> <databits> <stopbits> <parity>")
            return 0
        uart_passthrough(int(args[1]), int(args[2]), int(args[3]))
        #uart_passthrough_refined(args[1], args[2], args[3])
        #uart_passthrough_refined(args[1], args[2], args[3], args[4], args[5], args[6])
        return 0
    
    elif command.startswith("config "):
        # Expected input: BUSSide> uart config 8 n 1
        args = command.split() # ['config', '8', 'n', '1']
        if len(args) != 4:
            print("Usage: uart config <databits> <parity> <stopbits>")
            print("Example: uart config 8 n 1")
            return 0
        
        # Pass the arguments directly to our updated function
        uart_config(args[1], args[2], args[3])
        return 0
    
    elif command == "status":
        uart_get_status()
        return 0
    
    else:
        return None
