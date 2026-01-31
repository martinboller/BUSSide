#!/usr/bin/env python3

"""
BUSSide JTAG Client Script

This script provides functions to interact with JTAG devices using the BUSSide hardware.
It supports discovering JTAG pinouts.
"""

import bs


def jtag_discover_pinout():
    """
    Discover available JTAG interfaces (pinouts) on the BUSSide device.

    Returns:
        tuple: (reply_length, reply_args) or None if timed out.
    """
    print("+++ Sending jtag pinout discovery command")

    request_args = []
    bs.NewTimeout(30)
    rv = bs.requestreply(13, request_args)
    if rv is None:
        print("--- JTAG Discovery Timed Out")
        return None

    bs_reply_length, bs_reply_args = rv

    num_found = bs_reply_args[0]
    if num_found > 0:
        print(f"+++ {num_found} JTAG interface(s) FOUND")

        # JTAG results are grouped in sets of 5: TCK, TMS, TDI, TDO, nTRST
        # Values start from index 1
        for i in range(num_found):
            base = 1 + (i * 5)
            print(f"--- Interface #{i+1}:")
            print(f"    TCK:   GPIO {bs_reply_args[base]}")
            print(f"    TMS:   GPIO {bs_reply_args[base+1]}")
            print(f"    TDI:   GPIO {bs_reply_args[base+2]}")
            print(f"    TDO:   GPIO {bs_reply_args[base+3]}")
            print(f"    nTRST: GPIO {bs_reply_args[base+4]}")
    else:
        print("--- No JTAG interfaces discovered.")

    print("+++ SUCCESS")
    return (bs_reply_length, bs_reply_args)


def doCommand(command):
    """
    Main command dispatcher for JTAG operations.

    Args:
        command (str): The command string to execute.

    Returns:
        int or None: 0 on success, None on invalid command.
    """
    # .startswith is cleaner and more 'Pythonic' than .find() == 0
    if command.strip() == "discover pinout":
        jtag_discover_pinout()
        return 0
    else:
        return None
