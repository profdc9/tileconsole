#!/usr/bin/env python3
"""Reads a USB log the board saved as /sd/usblogNN.bin (src/usblog.h has the format) and prints it as a timeline.

    python3 -I tools/usblog_decode.py usblog00.bin [--last N] [--skip-isr] [--brief]

--brief prints the story, one line per control request or event with its outcome and the receive timeouts that came with it,
instead of every record: times are seconds since the first record.
"""
import struct
import sys

INTS = ["HOST_CONN_DIS", "HOST_RESUME", "HOST_SOF", "TRANS_COMPLETE", "BUFF_STATUS", "ERROR_DATA_SEQ", "ERROR_RX_TIMEOUT",
        "ERROR_RX_OVERFLOW", "ERROR_BIT_STUFF", "ERROR_CRC", "STALL", "VBUS_DETECT", "BUS_RESET", "DEV_CONN_DIS", "DEV_SUSPEND",
        "DEV_RESUME_FROM_HOST", "SETUP_REQ", "DEV_SOF", "ABORT_DONE", "EP_STALL_NAK", "RX_SHORT_PACKET", "ENDPOINT_ERROR",
        "DEV_SM_WATCHDOG_FIRED", "EPX_STOPPED_ON_NAK"]
SIE_STATUS = {0: "VBUS_DETECTED", 4: "SUSPENDED", 10: "VBUS_OVER_CURR", 11: "RESUME", 12: "RX_SHORT_PACKET", 16: "CONNECTED", 17: "SETUP_REC",
              18: "TRANS_COMPLETE", 19: "BUS_RESET", 23: "ENDPOINT_ERROR", 24: "CRC_ERROR", 25: "BIT_STUFF_ERROR", 26: "RX_OVERFLOW",
              27: "RX_TIMEOUT", 28: "NAK_REC", 29: "STALL_REC", 30: "ACK_REC", 31: "DATA_SEQ_ERROR"}
SIE_CTRL = ["START_TRANS", "SEND_SETUP", "SEND_DATA", "RECEIVE_DATA", "STOP_TRANS", None, "PREAMBLE_EN", None, "SOF_SYNC", "SOF_EN",
            "KEEP_ALIVE_EN", "VBUS_EN", "RESUME", "RESET_BUS", None, "PULLDOWN_EN"]
SPEED = {0: "disconnected", 1: "low", 2: "full"}   # SIE_STATUS bits 9:8
HCD_EVENT = {0: "ATTACH", 1: "REMOVE", 2: "XFER_COMPLETE", 3: "FUNC_CALL"}
RESULT = {0: "SUCCESS", 1: "FAILED", 2: "STALLED", 3: "TIMEOUT", 4: "INVALID"}
PORT = {1: "port reset", 2: "port reset end", 3: "device close", 4: "abort transfer", 5: "clear stall"}
CLOSED = {0: "still running (not closed)", 1: "HANG", 2: "PANIC", 3: "HARD FAULT"}
REQUEST = {0: "GET_STATUS", 1: "CLEAR_FEATURE", 3: "SET_FEATURE", 5: "SET_ADDRESS", 6: "GET_DESCRIPTOR", 7: "SET_DESCRIPTOR", 8: "GET_CONFIGURATION",
           9: "SET_CONFIGURATION", 10: "GET_INTERFACE", 11: "SET_INTERFACE"}
HID_REQUEST = {1: "GET_REPORT", 2: "GET_IDLE", 3: "GET_PROTOCOL", 9: "SET_REPORT", 10: "SET_IDLE", 11: "SET_PROTOCOL"}
DESCRIPTOR = {1: "DEVICE", 2: "CONFIGURATION", 3: "STRING", 4: "INTERFACE", 5: "ENDPOINT", 0x21: "HID", 0x22: "HID REPORT", 0x29: "HUB"}


def bits(value, names):
    return "|".join(n for i, n in enumerate(names) if n and value >> i & 1) or "-"


def sie_status(v):
    s = [n for i, n in SIE_STATUS.items() if v >> i & 1]
    sp = SPEED.get(v >> 8 & 3)
    s.append("speed=" + (sp or "?"))
    s.append("line=%d" % (v >> 2 & 3))
    return "|".join(s)


def ep(x):
    d, e = x >> 8, x & 255
    return "dev %d ep %d%s" % (d, e & 15, " IN" if e & 128 else " OUT")


def setup(w0, w1):
    t, r, value, index, length = struct.unpack("<BBHHH", struct.pack("<II", w0, w1))
    cls = t >> 5 & 3
    name = (REQUEST if cls == 0 else HID_REQUEST if cls == 1 else {}).get(r, "request %d" % r)
    extra = ""
    if cls == 0 and r == 6:
        extra = " " + DESCRIPTOR.get(value >> 8, "type %d" % (value >> 8)) + " #%d" % (value & 255)
    return "%s %s%s wValue %04X wIndex %04X wLength %d%s" % ("IN" if t & 128 else "OUT", name, extra, value, index, length, "" if cls == 0 else " (class)")


def describe(e):
    t, typ, core, x, a, b, c, d = e
    if typ == 1:
        return "ISR  ints=%s  sie_status=%s  buf_status=%08X  epx_buf_ctrl=%08X  sie_ctrl=%s" % (bits(a, INTS), sie_status(b), c, d, bits(x, SIE_CTRL))
    if typ == 2:
        ev = HCD_EVENT.get(x >> 8, "event %d" % (x >> 8))
        if x >> 8 == 2:
            return "event XFER_COMPLETE dev %d ep %d%s  %s  %d bytes%s" % (x & 255, a & 15, " IN" if a & 128 else " OUT", RESULT.get(a >> 8 & 255, "?"), b, " (in isr)" if c else "")
        if x >> 8 in (0, 1):
            return "event %s dev %d  hub %d port %d speed %d%s" % (ev, x & 255, a & 255, a >> 8 & 255, a >> 16, " (in isr)" if c else "")
        return "event %s" % ev
    if typ == 3:
        return "transfer start %s  %d bytes  %s" % (ep(x), a, "accepted" if b else "REFUSED")
    if typ == 4:
        return "setup dev %d: %s  %s" % (x, setup(a, b), "accepted" if c else "REFUSED")
    if typ == 5:
        return "endpoint open %s  max packet %d  type %d interval %d  %s" % (ep(x), a, b & 3, b >> 8, "ok" if c else "FAILED")
    if typ == 6:
        what = PORT.get(a, "port %d" % a)
        return what + ("" if a < 3 else " " + (("dev %d" % x) if a == 3 else ep(x)))
    if typ == 7:
        return "HID mounted dev %d interface %d  report descriptor %d bytes" % (x >> 8, x & 255, a)
    if typ == 8:
        return "HID unmounted dev %d interface %d" % (x >> 8, x & 255)
    if typ == 9:
        return "DATA SEQUENCE ERROR flag seen by the task (#%d)  sie_status=%s  buf_status=%08X  epx_buf_ctrl=%08X" % (a, sie_status(b), c, d)
    if typ == 10:
        return "ISR record repeated %d more times over %d us" % (a, b)
    if typ == 11:
        return "USB HOST RESTARTED (%s), restart number %d" % ({1: "the root port was unplugged", 2: "an attach led nowhere", 3: "a receive timeout storm", 4: "a control transfer never completed", 5: "a buffer status for an endpoint with no transfer"}.get(a, a), b)
    if typ == 12:
        return "USB interrupt is %s; last turned off at %d by code at %08X; last turned on at %d" % ("OFF" if x else "on", a, b, c)
    if typ == 0xF0:
        return "PANIC \"%s\" arg %d (0x%X)" % (struct.pack("<II", a, b).split(b"\0")[0].decode("ascii", "replace"), c, c)
    if typ == 0xF1:
        return "HANG: sie_status=%s ints=%s buf_status=%08X, interrupt pending in %d of 1000 samples" % (sie_status(a), bits(b, INTS), c, d)
    if typ == 0xF2:
        return "HARD FAULT at %08X from %08X, CFSR %08X" % (a, b, c)
    return "type %d x=%04X a=%08X b=%08X c=%08X d=%08X" % (typ, x, a, b, c, d)


def brief(entries):
    t0 = entries[0][0]
    pending = None      # a transfer or a setup packet that has not completed yet: (text)
    timeouts = 0
    out = []

    def stamp(e):
        return "%9.3f" % (((e[0] - t0) & 0xFFFFFFFF) / 1e6)

    def flush(e):
        nonlocal timeouts
        if timeouts:
            out.append("%9s      ... %d receive timeouts" % ("", timeouts))
            timeouts = 0

    for e in entries:
        t, typ, core, x, a, b, c, d = e
        if typ == 1:
            if a & 1 << 6:
                timeouts += 1
            continue
        if typ == 10:
            timeouts += a
            continue
        if typ == 2 and x >> 8 == 2:
            res = RESULT.get(a >> 8 & 255, "?")
            line = "%s %s %d bytes -> %s" % (("data/status IN " if a & 128 else "data/status OUT")[:15], "ep%d" % (a & 15), b, "ok" if res == "SUCCESS" else res)
            if pending and (x & 255) == pending[0]:
                out.append(pending[1].replace("@", "-> " + ("ok" if res == "SUCCESS" else res) ))
                pending = None
            else:
                flush(e)
                out.append("%s    dev %d  %s" % (stamp(e), x & 255, line))
            continue
        if typ in (3, 4):
            flush(e)
            if pending:
                out.append(pending[1].replace("@", "(no completion)"))
            if typ == 4:
                pending = (x, "%s  dev %d  SETUP %s  @" % (stamp(e), x, setup(a, b)))
            else:
                pending = (x >> 8, "%s    dev %d  %s %s  @" % (stamp(e), x >> 8, "IN " if x & 128 else "OUT", "%d bytes requested" % a if a else "(status stage, 0 bytes)"))
            continue
        if typ == 5:
            continue
        flush(e)
        if pending and typ not in (1,):
            out.append(pending[1].replace("@", "(no completion)"))
            pending = None
        d_ = describe(e)
        out.append("%s  %s" % (stamp(e), d_))
    if pending:
        out.append(pending[1].replace("@", "(no completion)"))
    flush(entries[-1])
    return out


def main(argv):
    path = None
    last = None
    skip_isr = False
    brief_mode = False
    i = 1
    while i < len(argv):
        if argv[i] == "--last":
            last = int(argv[i + 1])
            i += 1
        elif argv[i] == "--skip-isr":
            skip_isr = True
        elif argv[i] == "--brief":
            brief_mode = True
        else:
            path = argv[i]
        i += 1
    if not path:
        print(__doc__)
        return 2
    with open(path, "rb") as f:
        raw = f.read()
    magic, version, size, count, total, closed = struct.unpack_from("<4sIIIII", raw, 0)
    if magic != b"USBL" or version != 1 or size != 24:
        print("not a USB log (or a different version)")
        return 1
    entries = [struct.unpack_from("<IBBHIIII", raw, 32 + 24 * n) for n in range(min(count, (len(raw) - 32) // 24))]
    print("%s: %d events kept of %d, ended by: %s" % (path, len(entries), total, CLOSED.get(closed, closed)))
    if entries:
        print("span %.3f ms" % (((entries[-1][0] - entries[0][0]) & 0xFFFFFFFF) / 1000.0))
    shown = entries[-last:] if last else entries
    if brief_mode and shown:
        print("\n".join(brief(shown)))
        return 0
    prev = None
    for e in shown:
        if skip_isr and e[1] == 1:
            continue
        delta = 0 if prev is None else (e[0] - prev) & 0xFFFFFFFF
        prev = e[0]
        print("%10d  +%-8d core %d  %s" % (e[0], delta, e[2], describe(e)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
