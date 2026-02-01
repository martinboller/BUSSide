#!/usr/bin/env python3

import cmd
import os
import sys
import readline
import traceback
import struct
import bs_i2c
import bs_uart
import bs_jtag
import bs
import bs_spi
import atexit

# Define history path
history_path = os.path.expanduser("~/.busside_history")

# 1. Load history if it exists
if os.path.exists(history_path):
    try:
        readline.read_history_file(history_path)
    except Exception:
        print("--- Warning: Could not read history file.")

# 2. Define the save function
def save_history():
    try:
        readline.write_history_file(history_path)
    except Exception as e:
        print(f"--- Warning: Could not save history: {e}")

# 3. Register the save function to run AUTOMATICALLY on exit
atexit.register(save_history)

# Define the command hierarchy
COMMAND_TREE = {
     'i2c': {
        'discover': ['pinout'],
        'scan': [],
        'dump': ['<addr> <len>'],
    },
    'jtag': {
        'discover': ['pinout']
    },
    'spi': {
        'discover': ['pinout'],
        'send': {
            'default': ['<cmdbyte1>'],
            '<cs>': ['<clk> <mosi> <miso> <cmdbyte1>']
        },
        'fuzz': ['<cs> <clk> <mosi> <miso>'],
        'flash': {
            'erase': {
                'sector': ['<address>']
            },
            'wp': ['enable', 'disable'],
            'read': {
                'id': ['<cs> <clk> <mosi> <miso>'],
                'uid': ['<cs> <clk> <mosi> <miso>'],
                'sreg1': ['<cs> <clk> <mosi> <miso>'],
                'sreg2': ['<cs> <clk> <mosi> <miso>']
            },
            'dump': ['<size> <outfile>'],
            'write': ['<size> <infile>']
        }
    },
    'uart': {
        'discover': {
            'data': [],
            'rx': [],
            'tx': ['<rx_gpio> <baudrate>']
        },
        'passthrough': {
            'auto': [],
            '\'<rx_gpio> <tx_gpio> <baudrate>\'': []
        },
    },
    'quit': [],
    'exit': []
}

def completer(text, state):
    buffer = readline.get_line_buffer()
    parts = buffer.lstrip().split()
    
    # If the buffer ends in a space, we are looking for the NEXT word
    if buffer.endswith(' '):
        parts.append('')

    node = COMMAND_TREE
    
    # Navigate the tree based on parts already typed
    for i in range(len(parts) - 1):
        word = parts[i]
        if isinstance(node, dict):
            if word in node:
                node = node[word]
            # Handle cases where a key might be a placeholder like <cs>
            elif list(node.keys())[0].startswith('<'):
                node = node[list(node.keys())[0]]
            else:
                return None
        elif isinstance(node, list):
            # We've reached a terminal list of options/hints
            return None
        else:
            return None

    # Generate matches based on the current node
    options = []
    if isinstance(node, dict):
        options = [k + ' ' for k in node.keys() if k.startswith(text)]
    elif isinstance(node, list):
        options = [item + ' ' for item in node if item.startswith(text)]

    if state < len(options):
        return options[state]
    return None

readline.set_completer(completer)
readline.parse_and_bind("tab: complete")

from click import command
# Ensure the Client/ directory is on sys.path when running from repo root
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "Client"))


sequence_number = 5

if len(sys.argv) != 2:
    print("Usage: %s <serdevice>" % (sys.argv[0]))
    sys.exit(0)

device = sys.argv[1]

def printHelp():
    print("+++ The BUSSide accepts the following commands")
    print("+++")
    print("I2C Commands:")
    print("+++ BUSSide> i2c discover pinout")
    print("+++ BUSSide> i2c discover slaves <sdaPin> <sclPin>")
    print(
        "+++ BUSSide> i2c flash dump <sdaPin> <sclPin> <slaveAddress> <addressLength> <size> <outfile>"
    )
    print(
        "+++ BUSSide> i2c flash write <sdaPin> <sclPin> <slaveAddress> <addressLength> <size> <infile>"
    )
    print("+++")
    print("JTAG Commands:")
    print("+++ BUSSide> jtag discover pinout")
    print("+++")
    print("SPI Commands:")
    print("+++ BUSSide> spi discover pinout")
    print("+++ BUSSide> spi send default <cmdbyte1> ....")
    print("+++ BUSSide> spi send <cs> <clk> <mosi> <miso> <cmdbyte1> ....")
    print("+++ BUSSide> spi fuzz <cs> <clk> <mosi> <miso>")
    print("+++ BUSSide> spi flash erase sector <address>")
    print("+++ BUSSide> spi flash wp enable|disable")
    print("+++ BUSSide> spi flash read id [<cs> <clk> <mosi> <miso>]")
    print("+++ BUSSide> spi flash read uid [<cs> <clk> <mosi> <miso>]")
    print("+++ BUSSide> spi flash read sreg1 [<cs> <clk> <mosi> <miso>]")
    print("+++ BUSSide> spi flash read sreg2 [<cs> <clk> <mosi> <miso>]")
    print("+++ BUSSide> spi flash dump <size> <outfile>")
    print("+++ BUSSide> spi flash write <size> <infile>")
    print("+++")
    print("UART Commands:")
    print("+++ BUSSide> uart discover data")
    print("+++ BUSSide> uart discover rx")
    print("+++ BUSSide> uart discover tx <rx_gpio> <baudrate>")
    print("+++ BUSSide> uart passthrough auto")
    print("+++ BUSSide> uart passthrough <rx_gpio> <tx_gpio> <baudrate>")
    print("+++")
    print("Other Commands:")
    print("+++ BUSSide> help - Print this help message")
    print("+++ BUSSide> Type quit, exit or hit Ctrl+D to exit.")
    print("+++")
    print("BUSSide Shell")

def doCommand(command):
    # STEP 1: Check for exit immediately
    # This prevents the print(), device reset, and the hardware sync from ever running
    if command.strip().lower() in ["quit", "exit"]:
        return -1
    
    if command.strip().lower() == "help":
        printHelp()
        return True # Return True so the main loop knows it was handled

    # 2. Hardware Commands (Reset + Sync)
    print(f"+++ Resetting and Syncing NodeMCU for <{command}>...")
    
    # Trigger the hardware reset
    bs.ResetDevice()
    
    # Perform the handshake
    bs.FlushInput()
    bs.NewTimeout(30)
    sync_result = bs.requestreply(0, [0x12345678])
    
    if sync_result is None:
        print("--- Error: Device failed to sync after reset.")
        return None
    # STEP 2: Now that we know it's a real command, perform sync
    # print("+++ Syncing with BUSSide before command execution...")
    bs.FlushInput()
    bs.NewTimeout(30)
   
    sync_result = bs.requestreply(0, [0x12345678])  # BS_ECHO
    if sync_result is None:
        return None
    
    # STEP 3: Route to sub-modules
    if command.find("spi ") == 0:
        return bs_spi.doCommand(command[4:])
    elif command.find("i2c ") == 0:
        return bs_i2c.doCommand(command[4:])
    elif command.find("uart ") == 0:
        return bs_uart.doCommand(command[5:])
    elif command.find("jtag ") == 0:
        return bs_jtag.doCommand(command[5:])
    else:
        return None

try:
    with open("/tmp/BUSSide.seq", "rb") as f:
        d = f.read(4)
        if len(d) == 4:
            (seq,) = struct.unpack("<I", d)
            bs.set_sequence_number(seq)
except Exception:
    pass

rv = bs.Connect(device)
if rv is None:
    print("--- Couldn't connect.")
    print("--- Unplug the BUSSide USB cable. Wait a few seconds.")
    print("--- Plug it in again. Then restart the software.")
    sys.exit(1)

# print("+++")
print("+++ Welcome to the BUSSide")
# print("+++ By Dr Silvio Cesare of InfoSect")
# print("+++")
printHelp()
# print("+++")

while True:
    try:
        # 1. Capture the input
        command = input("BUSSide> ").strip()
        if not command:
            continue
            
        # 2. Execute
        rv = doCommand(command)

        # 3. Evaluate return value
        if rv is None:
            printHelp()
        elif rv == -1:
            # User typed 'quit' or 'exit'
            break

    except KeyboardInterrupt:
        # User hit Ctrl+C during input OR during doCommand
        print("\n--- Interrupted. (Type 'quit' to exit safely)")
        # We continue here so a stray Ctrl+C doesn't kill your whole session
        continue 
    except EOFError:
        # User hit Ctrl+D
        break
    except Exception:
        print("\n--- ERROR: Unexpected Exception:")
        traceback.print_exc()
        continue

# The single, clean exit point
print("Cleaning up and exiting... Ciao!")
# If you are using a history file, it saves automatically here if using atexit
sys.exit(0)

# Turn off LED blinking when exiting normally (not quit)
bs.set_led_blink(0)
print("Ciao!")
