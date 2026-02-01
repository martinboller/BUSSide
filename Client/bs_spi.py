#!/usr/bin/env python3

"""
BUSSide SPI Client Script

This script provides functions to interact with SPI devices using the BUSSide hardware.
It supports discovering SPI pinouts, reading/writing flash memory, fuzzing commands,
and various SPI operations like reading IDs, status registers, and unique IDs.
"""

import bs
import struct

BLOCKSIZE = 1024  # Block size for dumping data in bytes
WRITEBLOCKSIZE = 256  # Block size for writing data in bytes


def dumpSPI(size, skip):
    """
    Dump data from SPI device.

    Args:
        size (int): Size of data to dump in bytes.
        skip (int): Offset to start dumping from.

    Returns:
        bytes: The dumped data, or None if failed.
    """
    request_args = [size, skip, 1000000]
    rv = bs.requestreply(1, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    data = b""
    # Python 3: // for integer division
    for i in range(bs_reply_length // 4):
        data = data + struct.pack("<I", bs_reply_args[i])
    return data


def spi_dump_flash(dumpsize, outfile):
    """
    Dump SPI flash memory to a file.

    Args:
        dumpsize (int): Total size to dump in bytes.
        outfile (str): Output file path.

    Returns:
        tuple or None: (1, 1) on success, None on failure.
    """
    bs.NewTimeout(5)
    skip = 0
    print("+++ Dumping SPI")
    try:
        with open(outfile, "wb") as f:
            while dumpsize > 0:
                size = min(dumpsize, BLOCKSIZE)
                data = dumpSPI(size, skip)
                if data is None:
                    print("--- Timeout during dump")
                    return None
                f.write(data)
                f.flush()
                skip += size
                dumpsize -= size
        print("+++ SPI Dump Command Successfully Completed\n")
    except Exception as e:
        print(f"--- File Error: {e}")
    return (1, 1)


def spi_read_id():
    """
    Read SPI device ID.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI read ID command")
    request_args = [1000000]
    rv = bs.requestreply(17, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    v1 = bs_reply_args[0]
    v2 = bs_reply_args[1]
    v3 = bs_reply_args[2]
    print("+++ SPI ID %.2x%.2x%.2x" % (v1, v2, v3))
    print("+++ SPI Read ID Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def writeSPI(size, skipsize, data):
    """
    Write data to SPI device.

    Args:
        size (int): Size of data to write in bytes.
        skipsize (int): Offset to start writing at.
        data (list): List of uint32 words to write.

    Returns:
        tuple: Reply from the BUSSide device.
    """
    # Python 3: Ensure integer division for range
    num_data_ints = size // 4
    request_args = [0] * (3 + num_data_ints)
    request_args[0] = size
    request_args[1] = skipsize
    request_args[2] = 1000000
    for i in range(num_data_ints):
        request_args[3 + i] = data[i]
    rv = bs.requestreply(37, request_args)
    return rv


def spi_flash(dumpsize, infile):
    """
    Write data from a file to SPI flash memory.

    Args:
        dumpsize (int): Total size to write in bytes.
        infile (str): Input file path.

    Returns:
        tuple or None: (1, 1) on success, None on failure.
    """
    bs.NewTimeout(5)
    skip = 0
    print("+++ Writing SPI")
    try:
        with open(infile, "rb") as f:
            while dumpsize > 0:
                size = min(dumpsize, WRITEBLOCKSIZE)
                f.seek(skip)
                rawdata = f.read(size)

                # Ensure we have enough data to fill 4-byte chunks
                num_chunks = size // 4
                data = [0] * num_chunks
                for i in range(num_chunks):
                    # Python 3: indexing bytes returns ints directly
                    a = rawdata[4 * i + 0]
                    b = rawdata[4 * i + 1]
                    c = rawdata[4 * i + 2]
                    d = rawdata[4 * i + 3]
                    data[i] = (d << 24) + (c << 16) + (b << 8) + a

                rv = writeSPI(size, skip, data)
                if rv is None:
                    print("--- Timeout during write")
                    return None
                skip += size
                dumpsize -= size
        print("+++ SPI Flash Command Successfully Completed\n")
    except Exception as e:
        print(f"--- File Error: {e}")
    return (1, 1)


def spi_fuzz(cs, clk, mosi, miso):
    """
    Fuzz SPI commands to discover valid ones.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending spi fuzz command")
    request_args = [1000000, cs, clk, mosi, miso]
    bs.NewTimeout(60)
    rv = bs.requestreply(35, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv

    n = bs_reply_length // (4 * 6)
    print("+++ FOUND %d SPI commands" % (n))
    for i in range(n):
        cmd = bs_reply_args[i * 6 + 0]
        v1 = bs_reply_args[i * 6 + 1]
        v2 = bs_reply_args[i * 6 + 2]
        v3 = bs_reply_args[i * 6 + 3]
        v4 = bs_reply_args[i * 6 + 4]
        v5 = bs_reply_args[i * 6 + 5]
        print("+++ SPI command FOUND")
        print("+++ SPI command %.2x" % (cmd))
        print("+++ SPI v1 %.2x" % (v1))
        print("+++ SPI v2 %.2x" % (v2))
        print("+++ SPI v3 %.2x" % (v3))
        print("+++ SPI v4 %.2x" % (v4))
        print("+++ SPI v5 %.2x" % (v5))
    print("+++ SPI Fuzz Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_discover_pinout():
    """
    Discover available SPI interfaces (pinouts) on the BUSSide device.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending spi discover pinout command")
    request_args = [1000000]
    bs.NewTimeout(60)
    rv = bs.requestreply(29, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv

    n = bs_reply_length // (4 * 4)
    print("+++ FOUND %d SPI interfaces" % (n))
    for i in range(n):
        cs = bs_reply_args[i * 4 + 0]
        clk = bs_reply_args[i * 4 + 1]
        mosi = bs_reply_args[i * 4 + 2]
        miso = bs_reply_args[i * 4 + 3]
        print("+++ SPI interface FOUND")
        print("+++ SPI CS at GPIO %i" % (cs))
        print("+++ SPI CLK at GPIO %i" % (clk))
        print("+++ SPI MOSI at GPIO %i" % (mosi))
        print("+++ SPI MISO at GPIO %i" % (miso))
    print("+++ SPI Discover Pinout Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_streg1(cs, clk, mosi, miso):
    """
    Read SPI status register 1.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI command")
    request_args = [1000000, cs, clk, mosi, miso, 2, 0x05, 0x00]
    rv = bs.requestreply(3, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    for i in range(1, 2):
        print("+++ STATUS REGISTER 1: %.2x" % (bs_reply_args[i]))
    print("+++ SPI Read Status Register 1 Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_streg2(cs, clk, mosi, miso):
    """
    Read SPI status register 2.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI command")
    request_args = [1000000, cs, clk, mosi, miso, 2, 0x35, 0x00]
    rv = bs.requestreply(3, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    for i in range(1, 2):
        print("+++ STATUS REGISTER 2: %.2x" % (bs_reply_args[i]))
    print("+++ SPI Read Status Register 2 Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_readuid(cs, clk, mosi, miso):
    """
    Read SPI unique ID.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Syncing with BUSSide before SPI read UID...")
    bs.NewTimeout(30)  # Increase timeout for sync check
    # Quick sync check with echo command
    sync_result = bs.requestreply(0, [0x12345678])  # BS_ECHO with test data
    if sync_result is None:
        print("--- Sync failed - device not responsive")
        return None
    print("+++ Device synced successfully")
    
    print("+++ Sending SPI command")
    request_args = [
        1000000,
        cs,
        clk,
        mosi,
        miso,
        13,
        0x4B,
        0x00,
        0x00,
        0x00,
        0x00,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    ]
    rv = bs.requestreply(3, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    for i in range(5, 13):
        print("+++ UID: %.2x" % (bs_reply_args[i]))
    print("+++ SPI Read UID Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def doSendCommand(cs, clk, mosi, miso, args):
    """
    Send a custom SPI command.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.
        args (list): List of command arguments as strings.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI command")
    n = len(args)
    # Correctly initialize list with integers
    request_args = [0] * (6 + n)
    request_args[0] = 1000000
    request_args[1] = cs
    request_args[2] = clk
    request_args[3] = mosi
    request_args[4] = miso
    request_args[5] = n
    for i in range(n):
        try:
            # Handle hex (0x9F) or decimal
            request_args[6 + i] = int(args[i], 0)
        except ValueError:
            print(f"--- Error: '{args[i]}' is not a valid number")
            return None

    rv = bs.requestreply(3, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    for i in range(min(n, len(bs_reply_args))):
        print("+++ SPI Response: %.2x" % (bs_reply_args[i]))
    print("+++ SPI Send Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_wp_enable(cs, clk, mosi, miso):
    """
    Enable SPI write protection.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI write protect commands")
    request_args = [1000000, cs, clk, mosi, miso]
    rv = bs.requestreply(41, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    print("+++ SPI Write Protect Command Enable Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_wp_disable(cs, clk, mosi, miso):
    """
    Disable SPI write protection.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI write protect commands")
    request_args = [1000000, cs, clk, mosi, miso]
    rv = bs.requestreply(39, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    print("+++ SPI Write Protect Disable Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_bb_read_id(cs, clk, mosi, miso):
    """
    Read SPI device ID using bit-banging.

    Args:
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI read ID command")
    request_args = [1000000, cs, clk, mosi, miso]
    rv = bs.requestreply(31, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    v1 = bs_reply_args[0]
    v2 = bs_reply_args[1]
    v3 = bs_reply_args[2]
    print("+++ SPI ID %.2x%.2x%.2x" % (v1, v2, v3))
    print("+++ SPI Read ID Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def spi_erase_sector(skipsize, cs, clk, mosi, miso):
    """
    Erase a sector in SPI flash memory.

    Args:
        skipsize (int): Sector address to erase.
        cs (int): Chip select GPIO pin.
        clk (int): Clock GPIO pin.
        mosi (int): MOSI GPIO pin.
        miso (int): MISO GPIO pin.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending SPI erase sector command")
    request_args = [1000000, skipsize, cs, clk, mosi, miso]
    rv = bs.requestreply(27, request_args)
    if rv is None:
        return None
    bs_reply_length, bs_reply_args = rv
    print("+++ SPI Erase Sector Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def doFlashCommand(command):
    """
    Handle flash-related SPI commands.

    Args:
        command (str): The flash command string.

    Returns:
        int or None: 0 on success, None on invalid command.
    """
    if command.find("read id") == 0:
        args = command[7:].split()
        if len(args) == 0:
            spi_read_id()
        elif len(args) == 4:
            a0 = int(args[0])
            a1 = int(args[1])
            a2 = int(args[2])
            a3 = int(args[3])
            spi_bb_read_id(a0, a1, a2, a3)
            return 0
        else:
            return None
    elif command.find("read sreg1") == 0:
        args = command[10:].split()
        if len(args) == 0:
            spi_streg1(9, 6, 8, 7)
            return 0
        elif len(args) == 4:
            a0 = int(args[0])
            a1 = int(args[1])
            a2 = int(args[2])
            a3 = int(args[3])
            spi_streg1(a0, a1, a2, a3)
            return 0
        else:
            return None
    elif command.find("read sreg2") == 0:
        args = command[10:].split()
        if len(args) == 0:
            spi_streg2(9, 6, 8, 7)
            return 0
        elif len(args) == 4:
            a0 = int(args[0])
            a1 = int(args[1])
            a2 = int(args[2])
            a3 = int(args[3])
            spi_streg2(a0, a1, a2, a3)
            return 0
        else:
            return None
    elif command.find("read uid") == 0:
        args = command[8:].split()
        if len(args) == 0:
            spi_readuid(9, 6, 8, 7)
        elif len(args) == 4:
            a0 = int(args[0])
            a1 = int(args[1])
            a2 = int(args[2])
            a3 = int(args[3])
            spi_readuid(a0, a1, a2, a3)
            return 0
    elif command == "wp enable":
        spi_wp_enable(9, 6, 8, 7)
        return 0
    elif command == "wp disable":
        spi_wp_disable(9, 6, 8, 7)
        return 0
    elif command.find("write ") == 0:
        args = command[5:].split()
        if len(args) != 2:
            return None
        spi_flash(int(args[0]), args[1])
        return 0
    elif command.find("dump ") == 0:
        args = command[5:].split()
        if len(args) != 2:
            return None
        spi_dump_flash(int(args[0]), args[1])
        return 0
    elif command.find("erase sector ") == 0:
        args = command[12:].split()
        if len(args) == 1:
            spi_erase_sector(int(args[0]), 9, 6, 8, 7)
            return 0
        else:
            return None
    else:
        return None


def doCommand(command):
    """
    Main command dispatcher for SPI operations.

    Args:
        command (str): The command string to execute.

    Returns:
        int or None: 0 on success, None on invalid command.
    """
    if command.find("flash ") == 0:
        doFlashCommand(command[6:])
        return 0
    elif command.find("send default ") == 0:
        args = command[12:].split()
        if len(args) < 5:
            return None
        doSendCommand(9, 6, 8, 7, args)
        return 0
    elif command.find("send ") == 0:
        args = command[4:].split()
        if len(args) < 4:
            return None
        doSendCommand(int(args[0]), int(args[1]), int(args[2]), int(args[3]), args[4:])
        return 0
    elif command == "discover pinout":
        spi_discover_pinout()
        return 0
    elif command.find("fuzz ") == 0:
        args = command[4:].split()
        if len(args) == 4:
            spi_fuzz(int(args[0]), int(args[1]), int(args[2]), int(args[3]))
            return 0
        else:
            return None
    else:
        return None
