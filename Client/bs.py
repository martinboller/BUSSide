#!/usr/bin/env python3

"""
Client/bs.py
Low-level Python helper functions for communicating with the BUSSide hardware
over a serial port. This module implements simple framing, CRC checks, a
sequence-number based request/reply protocol, and some helper utilities for
terminal key handling.

The code is intentionally small and synchronous; higher-level clients use
these primitives to implement commands and tests.
"""

import time
import struct
import os
import sys
import serial
import binascii
import select
import termios
import fcntl

# Module-global state used by the simple client. These are mutated by
# Connect(), requestreply(), and the sequence-number helpers.
mydevice = None  # path to serial device (e.g. /dev/ttyUSB0)
mytimeout = 10  # default read timeout in seconds
myserial = None  # instance of serial.Serial once connected
sequence_number = 5  # current outgoing sequence number (persisted to /tmp)
oldterm = 0  # saved terminal attributes for keys_* helpers
oldflags = 0  # saved file flags for stdin
baudrate = 500000  # serial baud rate to open the device with


def keys_isData():
    """Return True if a character is available on stdin (non-blocking).

    Uses select.select to check for input readiness without blocking.
    This helper is intended for simple interactive shells that may poll
    stdin for user keystrokes.
    """
    return select.select([sys.stdin], [], [], 0) == ([sys.stdin], [], [])


def keys_init():
    """Put the terminal into a raw-ish mode and make stdin non-blocking.

    This function modifies terminal attributes so that characters are
    available immediately (no canonical line buffering) and disables echo.
    It also sets the file descriptor to non-blocking mode. The previous
    terminal attributes and flags are saved to module globals so that
    keys_cleanup() can restore them later.
    """
    global oldterm
    global oldflags

    fd = sys.stdin.fileno()
    newattr = termios.tcgetattr(fd)
    # c_lflag index 3 contains ICANON and ECHO bits among others
    newattr[3] = newattr[3] & ~termios.ICANON
    newattr[3] = newattr[3] & ~termios.ECHO
    termios.tcsetattr(fd, termios.TCSANOW, newattr)

    # Save original settings so they can be restored
    oldterm = termios.tcgetattr(fd)
    oldflags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, oldflags | os.O_NONBLOCK)


def keys_getchar():
    """Read a single character from stdin if available, otherwise None.

    This is a non-blocking convenience wrapper around keys_isData() and
    sys.stdin.read(1). Returns a single-character string or None.
    """
    if keys_isData():
        return sys.stdin.read(1)
    else:
        return None


def keys_cleanup():
    """Restore saved terminal attributes and blocking flags.

    This should be called after keys_init() to return the terminal to the
    state it was in before. It relies on `oldterm` and `oldflags` globals
    previously set by keys_init().
    """
    global oldterm
    global oldflags

    fd = sys.stdin.fileno()
    termios.tcsetattr(fd, termios.TCSAFLUSH, oldterm)
    fcntl.fcntl(fd, fcntl.F_SETFL, oldflags)


def set_sequence_number(seq):
    """Set the outgoing sequence number to `seq`.

    The sequence number is used to match replies with requests. This helper
    allows external code to recover session ids from persistent storage or
    to resume a previously used sequence.
    """
    global sequence_number

    sequence_number = seq


def get_sequence_number():
    """Return the current sequence number (integer)."""
    global sequence_number

    return sequence_number


def next_sequence_number():
    """Advance the sequence number and persist it to /tmp/BUSSide.seq.

    The sequence number is advanced modulo 2^30 to keep it bounded, then
    the new value is written (little-endian) to a temporary file. Writing
    to a file allows other processes or subsequent runs to pick up the
    current sequence value for debugging or crash recovery.
    """
    global sequence_number

    sequence_number = (sequence_number + 1) % (1 << 30)
    with open("/tmp/BUSSide.seq", "wb") as f:
        f.write(struct.pack("<I", sequence_number))


def FlushInput():
    """Flush any pending input from the serial device's receive buffer.

    Delegates to the pySerial `flushInput()` method (platform-dependent
    name). This is useful after opening the port to discard any leftover
    bytes (boot messages, garbled data) before starting protocol frames.
    """
    global myserial

    myserial.flushInput()


def Sync():
    """Wait for the device to send the two-byte sync marker 0xFE 0xCA.

    The BUSSide firmware emits a short "welcome" sequence; this helper
    scans incoming bytes until it finds the two magic bytes in order. It
    returns True on success and False if the configured timeout elapses.
    """
    global myserial, mytimeout
    # Give the hardware a moment to finish its "Welcome" speech
    # and look for the magic bytes
    start_time = time.time()
    print("+++ Hunting for sync bytes (0xFE 0xCA)...")

    while (time.time() - start_time) < mytimeout:
        char1 = myserial.read(1)
        if not char1:
            # Nothing read: continue polling until timeout
            continue

        # `char1` is a bytes object; index 0 gives the integer value
        if char1[0] == 0xFE:
            # If first byte matches, attempt to read the second byte
            char2 = myserial.read(1)
            if char2 and char2[0] == 0xCA:
                print("+++ Sync Achieved!")
                return True
    print("--- Sync Timeout: Magic bytes not found.")
    return False


def requestreply(command, request_args, nretries=10):
    """Send a framed request and wait for a validated reply.

    Frame format used by this client (little-endian 32-bit words):
      - 4 bytes: command id
      - 4 bytes: payload length in bytes
      - 4 bytes: sequence number
      - 4 bytes: CRC32 (of the header+sequence+payload when sending)
      - payload: N arguments, each 4 bytes (uint32)

    The function retries `nretries` times on transient failures. On each
    attempt it will increment the sequence number (persisted by
    next_sequence_number()), build the CRC, send the frame prefixed by the
    2-byte sync (0xFE 0xCA), then wait for a reply which it validates by
    length, CRC and sequence number. On success it returns a tuple
    (reply_length, reply_args_list) where `reply_args_list` is a list of
    decoded uint32 values. Returns None after exhausting retries.
    """
    global myserial
    global mydevice
    global mytimeout

    # Ensure we have an open serial connection; attempt Connect() if not
    if myserial is None:
        rv = Connect()
        if rv is None:
            return None

    for i in range(nretries):
        if i > 0:
            print("+++ Retransmitting %d/10" % (i))
            # After several retries, try flushing and reconnecting to
            # recover from a stuck device or corrupted state.
            if i > 3:
                FlushInput()
                time.sleep(5)
                rv = Connect(mydevice, mytimeout, 0)
                if rv is None:
                    continue

        # build the frame pieces
        bs_sync = b"\xfe\xca"  # always prefix frames with this marker
        bs_command = struct.pack("<I", command)
        # payload length in bytes (4 bytes per uint32 argument)
        bs_command_length = struct.pack("<I", len(request_args) * 4)
        bs_request_args = b""
        for j in range(len(request_args)):
            bs_request_args += struct.pack("<I", request_args[j])

        # calculate crc over command + length + sequence + zero + args
        # Note: a zero placeholder is used in the pre-CRC layout.
        request = bs_command
        request += bs_command_length
        saved_sequence_number = get_sequence_number()
        # Advance the saved sequence for the next outgoing request
        next_sequence_number()
        request += struct.pack("<I", saved_sequence_number)
        request += struct.pack("<I", 0x00000000)
        request += bs_request_args
        crc = binascii.crc32(request) & 0xFFFFFFFF

        # now build the on-wire frame: command, length, seq, crc, args
        request = bs_command
        request += bs_command_length
        request += struct.pack("<I", saved_sequence_number)
        request += struct.pack("<I", crc)
        request += bs_request_args
        # send the sync marker plus the serialized frame
        myserial.write(bs_sync + request)
        myserial.flush()

        # wait for device to send the 0xFE 0xCA sync back before reading
        if not Sync():
            # device didn't respond with sync marker; retry
            continue

        # read reply header fields (all little-endian 32-bit words)
        bs_command = myserial.read(4)
        if len(bs_command) != 4:
            continue
        bs_reply_length = myserial.read(4)
        if len(bs_reply_length) != 4:
            continue
        (reply_length,) = struct.unpack("<I", bs_reply_length)
        # sanity-check to avoid reading enormous lengths
        if reply_length > 65356:
            continue
        bs_sequence_number = myserial.read(4)
        if len(bs_sequence_number) != 4:
            continue
        (seq,) = struct.unpack("<I", bs_sequence_number)
        d = myserial.read(4)
        if len(d) != 4:
            continue
        (bs_checksum,) = struct.unpack("<I", d)

        # read reply payload (reply_length bytes, expected to be multiple
        # of 4 since arguments are 4-byte words)
        reply_args = b""
        if reply_length == 0:
            bs_reply_args = []
        else:
            # Prepare a list sized for the expected number of args
            bs_reply_args = list(range(reply_length // 4))
            fail = False
            for j in range(reply_length // 4):
                s = myserial.read(4)
                if len(s) != 4:
                    fail = True
                    break
                reply_args += s
                (bs_reply_args[j],) = struct.unpack("<I", s)
            if fail:
                # incomplete payload; try the whole request again
                continue

        # calculate checksum on the received parts (same layout as when
        # the device calculated it: command + length + seq + zero + args)
        reply = bs_command
        reply += bs_reply_length
        reply += bs_sequence_number
        reply += struct.pack("<I", 0x00000000)
        reply += reply_args
        crc = binascii.crc32(reply) & 0xFFFFFFFF

        # verify CRC and sequence number
        if crc != bs_checksum:
            # bad frame checksum
            continue
        if saved_sequence_number != seq:
            # sequence mismatch (stale/duplicate reply)
            continue

        # frame validated; return length and the decoded uint32 args list
        return (reply_length, bs_reply_args)

    # all retries exhausted
    return None


def getSerial():
    """Return the current pySerial object (or None if not connected)."""
    global myserial

    return myserial


def NewTimeout(ltimeout):
    """Change the global read timeout and update the open serial timeout.

    This updates the module-level `mytimeout` and, if a serial connection
    is currently open, sets the pySerial `timeout` attribute so reads
    reflect the new value immediately.
    """
    global myserial
    global mytimeout
    mytimeout = ltimeout
    if myserial:
        myserial.timeout = ltimeout  # Critical: Update the hardware timeout


def Connect(device, ltimeout=2, nretries=10):
    """Open the serial device and verify basic communication.

    Attempts to open `device` at the module `baudrate` and then performs a
    simple echo/handshake (request command 0 with no args) to verify the
    firmware is alive. `ltimeout` sets the per-read timeout; `nretries`
    controls how many attempts are made to open and validate the port.
    On success this function returns whatever `requestreply()` returned
    from the echo command (a tuple), otherwise None.
    """
    global myserial
    global mytimeout
    global mydevice

    print(f"+++ Connecting to the BUSSide on {device}")

    # Close any existing serial handle before (re)opening
    if myserial is not None:
        myserial.close()
        myserial = None

    mydevice = device
    mytimeout = ltimeout
    n = max(1, nretries)

    for i in range(n):
        try:
            # Pass DTR/RTS settings INSIDE the constructor to avoid toggles
            myserial = serial.Serial(
                mydevice,
                baudrate,
                timeout=mytimeout,
                dsrdtr=False,
                rtscts=False,
            )

            # Explicitly ensure lines are not asserted
            myserial.dtr = False
            myserial.rts = False

            # Many microcontrollers (e.g. ESP8266) reset when the port is
            # opened; wait briefly to allow the firmware to boot and emit
            # any welcome/sync bytes that Sync() will look for.
            time.sleep(2)

            FlushInput()

            if nretries > 0:
                print("+++ Sending echo command...")
                request_args = []
                # Send command 0 as a lightweight echo/ping with 1 retry
                rv = requestreply(0, request_args, 1)
                if rv is None:
                    print("--- Echo failed, retrying...")
                    myserial.close()
                    continue
                # print("+++ OK")
                return rv
            return (1, 1)
        except Exception as e:
            print(f"--- Connection Error: {e}")
            pass
    return None


def set_led_blink(interval_ms):
    """Set LED blink interval in milliseconds. 0 to stop blinking."""
    # print(f"+++ Setting LED blink interval to {interval_ms}ms")
    request_args = [interval_ms]
    rv = requestreply(45, request_args)  # BS_LED_BLINK = 45
    if rv is None:
        return None
    (bs_reply_length, bs_reply_args) = rv
    # print("+++ LED blink command sent")
    return (bs_reply_length, bs_reply_args)
