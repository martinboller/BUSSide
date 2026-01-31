#!/usr/bin/env python3

import os
import sys
# Ensure the Client/ directory is on sys.path when running from repo root
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "Client"))

import struct
import traceback
import bs_i2c
import bs_uart
import bs_jtag
import bs
import bs_spi

sequence_number = 5

if len(sys.argv) != 2:
    print("Usage: %s <serdevice>" % (sys.argv[0]))
    sys.exit(0)

device = sys.argv[1]


def printHelp():
    print("+++ The BUSSide accepts the following commands")
    print("+++")
    print("SPI Commands:")
    print("+++ > spi discover pinout")
    print("+++ > spi send default <cmdbyte1> ....")
    print("+++ > spi send <cs> <clk> <mosi> <miso> <cmdbyte1> ....")
    print("+++ > spi fuzz <cs> <clk> <mosi> <miso>")
    print("+++ > spi flash erase sector <address>")
    print("+++ > spi flash wp enable|disable")
    print("+++ > spi flash read id [<cs> <clk> <mosi> <miso>]")
    print("+++ > spi flash read uid [<cs> <clk> <mosi> <miso>]")
    print("+++ > spi flash read sreg1 [<cs> <clk> <mosi> <miso>]")
    print("+++ > spi flash read sreg2 [<cs> <clk> <mosi> <miso>]")
    print("+++ > spi flash dump <size> <outfile>")
    print("+++ > spi flash write <size> <infile>")
    print("+++")
    print("I2C Commands:")
    print("+++ > i2c discover pinout")
    print("+++ > i2c discover slaves <sdaPin> <sclPin>")
    print(
        "+++ > i2c flash dump <sdaPin> <sclPin> <slaveAddress> <addressLength> <size> <outfile>"
    )
    print(
        "+++ > i2c flash write <sdaPin> <sclPin> <slaveAddress> <addressLength> <size> <infile>"
    )
    print("+++")
    print("JTAG Commands:")
    print("+++ > jtag discover pinout")
    print("+++")
    print("UART Commands:")
    print("+++ > uart discover data")
    print("+++ > uart passthrough auto")
    print("+++ > uart discover rx")
    print("+++ > uart discover tx <rx_gpio> <baudrate>")
    print("+++ > uart passthrough <rx_gpio> <tx_gpio> <baudrate>")
    print("+++")
    print("Other Commands:")
    print("+++ > help - Print this help message")
    print("+++ > quit - quits the BUSSide program")
    print("+++")


def doCommand(command):
    # Perform initial sync with NodeMCU before any command
    print("+++ Syncing with BUSSide before command execution...")
    bs.FlushInput()
    bs.NewTimeout(30)
    sync_result = bs.requestreply(0, [0x12345678])  # BS_ECHO with test data
    if sync_result is None:
        print("--- Sync failed - device not responsive")
        return None
    print("+++ Device synced successfully")
    
    if command.find("spi ") == 0:
        return bs_spi.doCommand(command[4:])
    elif command.find("i2c ") == 0:
        return bs_i2c.doCommand(command[4:])
    elif command.find("uart ") == 0:
        return bs_uart.doCommand(command[5:])
    elif command.find("jtag ") == 0:
        return bs_jtag.doCommand(command[5:])
    elif command == "quit":
        return -1
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
        command = input("> ")
    except (EOFError, KeyboardInterrupt):
        break

    # Centralized handling: catch Ctrl-C and unexpected exceptions from
    # module command handlers and print helpful diagnostics.
    try:
        rv = doCommand(command)
    except KeyboardInterrupt:
        # User intentionally interrupted the running command.
        print("\\n--- Command interrupted by user (Ctrl-C).")
        continue
    except Exception:
        # Print a detailed traceback so the user can report the issue.
        print("\\n--- ERROR: Exception while executing command:")
        traceback.print_exc()
        continue

    if rv is None:
        printHelp()
    elif rv == -1:
        # Exit cleanly without trying to communicate with BUSSide
        print("Ciao!")
        sys.exit(0)

# Turn off LED blinking when exiting normally (not quit)
bs.set_led_blink(0)
print("Ciao!")
