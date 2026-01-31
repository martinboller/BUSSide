#!/usr/bin/env python3

import bs
import struct

BLOCKSIZE = 1024
WRITEBLOCKSIZE = 512


def i2c_discover_slaves(sda, scl):
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
    print("+++ SUCCESS\n")
    return (bs_reply_length, bs_reply_args)


def i2c_discover():
    print("+++ Sending i2c discover pinout command")
    request_args = []
    bs.NewTimeout(30)
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
    print("+++ SUCCESS\n")
    return (bs_reply_length, bs_reply_args)


def doFlashCommand(command):
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
        print("+++ SUCCESS\n")
        return (1, 1)


def i2c_write_flash(sda, scl, slave, alen, dumpsize, infile):
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
        print("+++ SUCCESS\n")
        return (1, 1)
