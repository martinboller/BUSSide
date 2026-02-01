#!/usr/bin/env python3

"""
BUSSide I2C Client Script

This script provides functions to interact with I2C devices using the BUSSide hardware.
It supports discovering I2C slaves, dumping and writing flash memory over I2C.
"""

import bs
import struct

BLOCKSIZE = 1024  # Block size for dumping data in bytes
WRITEBLOCKSIZE = 512  # Block size for writing data in bytes


def i2c_discover_slaves(sda, scl):
    """
    Discover I2C slave devices on the specified SDA and SCL pins.

    Args:
        sda (int): GPIO pin number for SDA.
        scl (int): GPIO pin number for SCL.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending i2c slave discovery command")
    request_args = [sda, scl]
    rv = bs.requestreply(5, request_args)
    if rv is None:
        return None
    (bs_reply_length, bs_reply_args) = rv

    # reply length is in bytes; each address is a 4-byte uint32
    nslave_addresses = bs_reply_length // 4
    print("+++ %d I2C slave addresses" % (nslave_addresses))
    for i in range(nslave_addresses):
        print("+++ I2C slave address FOUND at %i" % bs_reply_args[i])
    print("+++ I2C Discover Slaves Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def i2c_discover():
    """
    Discover available I2C interfaces (pinouts) on the BUSSide device.

    Returns:
        tuple: (reply_length, reply_args) or None if failed.
    """
    print("+++ Sending i2c discover pinout command")
    request_args = []
    bs.NewTimeout(30)  # Reduced timeout now that scanning is optimized
    rv = bs.requestreply(23, request_args)
    if rv is None:
        return None
    (bs_reply_length, bs_reply_args) = rv

    # Each interface reports two uint32 values (SDA, SCL)
    n = bs_reply_length // 8
    print("+++ FOUND %d I2C interfaces" % (n))
    for i in range(n):
        sda = bs_reply_args[i * 2 + 0]
        scl = bs_reply_args[i * 2 + 1]
        print("+++ I2C interface FOUND")
        print("+++ I2C SDA at GPIO %i" % (sda))
        print("+++ I2C SCL at GPIO %i" % (scl))
    print("+++ I2C Discover Pinout Command Successfully Completed\n")
    return (bs_reply_length, bs_reply_args)


def doFlashCommand(command):
    """
    Handle flash-related commands for I2C devices.

    Args:
        command (str): The flash command string.

    Returns:
        int or None: 0 on success, None on invalid command.
    """
    if command.find("dump ") == 0:
        args = command[5:].split()
        if len(args) != 6:
            return None
        i2c_dump_flash(
            int(args[0]),
            int(args[1]),
            int(args[2]),
            int(args[3]),
            int(args[4]),
            args[5],
        )
        return 0
    elif command.find("write ") == 0:
        args = command[6:].split()
        if len(args) != 6:
            return None
        i2c_write_flash(
            int(args[0]),
            int(args[1]),
            int(args[2]),
            int(args[3]),
            int(args[4]),
            args[5],
        )
        return 0
    else:
        return None


def doCommand(command):
    """
    Main command dispatcher for I2C operations.

    Args:
        command (str): The command string to execute.

    Returns:
        int or None: 0 on success, None on invalid command.
    """
    if command.find("flash ") == 0:
        doFlashCommand(command[6:])
        return 0
    elif command == "discover pinout":
        i2c_discover()
        return 0
    elif command.find("discover slaves ") == 0:
        args = command[16:].split()
        if len(args) != 2:
            return None
        i2c_discover_slaves(int(args[0]), int(args[1]))
        return 0
    else:
        return None


def writeI2C(sda, scl, slave, size, skip, alen, data):
    """
    Write data to an I2C slave device.

    Args:
        sda (int): GPIO pin for SDA.
        scl (int): GPIO pin for SCL.
        slave (int): I2C slave address.
        size (int): Size of data to write in bytes.
        skip (int): Offset to start writing at.
        alen (int): Address length.
        data (list): List of uint32 words to write.

    Returns:
        tuple: Reply from the BUSSide device.
    """
    # Preallocate a list of integers for the request arguments
    num_words = size // 4
    request_args = [0] * (6 + num_words)
    request_args[0] = slave
    request_args[1] = size
    request_args[2] = skip
    request_args[3] = sda
    request_args[4] = scl
    request_args[5] = alen
    for i in range(num_words):
        request_args[6 + i] = data[i]
    rv = bs.requestreply(25, request_args)
    return rv


def dumpI2C(sda, scl, slave, size, skip, alen):
    """
    Dump data from an I2C slave device.

    Args:
        sda (int): GPIO pin for SDA.
        scl (int): GPIO pin for SCL.
        slave (int): I2C slave address.
        size (int): Size of data to dump in bytes.
        skip (int): Offset to start dumping from.
        alen (int): Address length.

    Returns:
        bytes: The dumped data, or None if failed.
    """
    data = b""
    request_args = [slave, size, skip, sda, scl, alen]
    rv = bs.requestreply(9, request_args)
    if rv is None:
        return None
    (bs_reply_length, bs_reply_args) = rv
    # Assemble raw bytes from returned uint32 words
    for i in range(bs_reply_length // 4):
        data = data + struct.pack("<I", bs_reply_args[i])
    return data


def i2c_dump_flash(sda, scl, slave, alen, dumpsize, outfile):
    """
    Dump flash memory from an I2C device to a file.

    Args:
        sda (int): GPIO pin for SDA.
        scl (int): GPIO pin for SCL.
        slave (int): I2C slave address.
        alen (int): Address length.
        dumpsize (int): Total size to dump in bytes.
        outfile (str): Output file path.

    Returns:
        tuple or None: (1, 1) on success, None on failure.
    """
    skip = 0
    print("+++ Dumping I2C")
    with open(outfile, "wb") as f:
        while dumpsize > 0:
            if dumpsize < BLOCKSIZE:
                size = dumpsize
            else:
                size = BLOCKSIZE
            data = dumpI2C(sda, scl, slave, size, skip, alen)
            if data is None:
                print("Timeout")
                return None
            f.write(data)
            f.flush()
            # advance by the actual transferred size
            skip += size
            dumpsize -= size
        print("+++ I2C Dump Successfully Completed\n")
        return (1, 1)


def i2c_write_flash(sda, scl, slave, alen, dumpsize, infile):
    """
    Write data from a file to flash memory on an I2C device.

    Args:
        sda (int): GPIO pin for SDA.
        scl (int): GPIO pin for SCL.
        slave (int): I2C slave address.
        alen (int): Address length.
        dumpsize (int): Total size to write in bytes.
        infile (str): Input file path.

    Returns:
        tuple or None: (1, 1) on success, None on failure.
    """
    bs.NewTimeout(5)
    skip = 0
    print("+++ Writing I2C")
    with open(infile, "rb") as f:
        while dumpsize > 0:
            if dumpsize < WRITEBLOCKSIZE:
                size = dumpsize
            else:
                size = WRITEBLOCKSIZE
            f.seek(skip)
            rawdata = f.read(size)
            # Convert raw bytes into uint32 words. Python3: indexing bytes
            # yields ints, so `ord()` is not required. Use integer division
            # so sizes are ints.
            num_words = size // 4
            data = [0] * num_words
            for i in range(num_words):
                a = rawdata[4 * i + 0]
                b = rawdata[4 * i + 1]
                c = rawdata[4 * i + 2]
                d = rawdata[4 * i + 3]
                data[i] = (d << 24) + (c << 16) + (b << 8) + a
            rv = writeI2C(sda, scl, slave, size, skip, alen, data)
            if rv is None:
                print("Timeout")
                return None
            # advance by the actual transferred size
            skip += size
            dumpsize -= size
        print("+++ I2C Write Flash Command Successfully Completed\n")
        return (1, 1)
