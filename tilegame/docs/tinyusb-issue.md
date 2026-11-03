# rp2040 host: low-speed device behind a hub fails to enumerate if a full-speed device was enumerated first

Related: #4180 lists "low-speed device behind a full-speed hub (PREAMBLE)" as not validated on rp2040/rp2350.

## Summary
RP2350 host, one hub, a full-speed gamepad and a low-speed keyboard on it. With the same hub and the same two devices in the same
session:

- **gamepad enumerated first, keyboard second:** the keyboard's control requests get no answer; the controller retries the same
  packet for ever (receive timeouts, thousands a second) and enumeration never completes.
- **keyboard enumerated first, gamepad second:** both enumerate and both keep working.

The hub reports its ports lowest first after a host restart, so by moving the keyboard to a lower port than the gamepad I can make
a host restart (`tuh_deinit()` + `tuh_init()`) turn the failing order into the working one. `usblog23` shows one failure and
the recovery in a single run (annotated below). Three different hubs (bus-powered USB 3.0, self-powered, and one more) behave the same.
Power is ruled out (self-powered hub, bigger supply capacitor, 252 MHz).

## Setup
- RP2350B board, USB host on the native port, `BOARD_TUH_RHPORT 0`, full speed, OS none, `CFG_TUH_HUB 1`.
- TinyUSB 0.18.0 (Pico SDK 2.3.1, `86ad6e56c1700e85f1c5678607a762cfe3aa2f47`) is the version of the logs below, with `CFG_TUSB_DEBUG=2`.
  Also tried: 0.21.0 (`dae3f9a3`) and master `27ed59447`; see "Other versions".
- Firmware: https://github.com/profdc9/tileconsole, commit `285e0d2ef2db8a7d08bd4e313a2d87422c5ae292` (build options `-DPLAT_USB_HID=ON
  -DCPU_MHZ=504 -DTUSB_LOG=ON`); the log recorder is `tilegame/board/usblog_usb.c`.
- The RP2350 host is full speed only, so the hub runs at full speed and the keyboard is reached with preamble (`PREAMBLE_EN`).
  Linux, on a high-speed host, enumerates the same keyboard through the hub's TT without trouble.
- Keyboard: HID boot keyboard, bcdUSB 1.10, EP0 max packet 8. Gamepad: composite (two HID interfaces plus a vendor XInput interface).

## The log
`usblog23-annotated.txt` (in the repository linked at the end) walks through the run; short form:
```
 1.377  ATTACH hub 6 port 3           gamepad (dev 1)       ... enumerates, mounted at 1.87
 3.405  ATTACH hub 6 port 1           keyboard
 3.856  dev 0 GET_DESCRIPTOR 8   ok   3.857 SET_ADDRESS 2 ok
 3.859  dev 2 GET_DESCRIPTOR DEVICE 18: only the first 8-byte packet arrives (13 receive timeouts, then the stage completes
        as SUCCESS with 8 bytes; debug text: "on EP 80 with 8 bytes: OK"); the status stage times out 10 more times, then completes
 3.861  dev 2 GET_DESCRIPTOR CONFIGURATION 9  (the SETUP gets a receive timeout and never completes; the controller re-sends it, ~9000 timeouts/s)
 4.192  host restarted (my firmware does this after 3000 timeouts)
 5.116  ATTACH hub 6 port 1           keyboard now first    all requests ok, mounted at 5.58
 5.626  ATTACH hub 6 port 3           gamepad second        all requests ok, mounted at 6.11
```
`tusb23.txt` is TinyUSB's debug output of the same run (its clock starts about 0.16 s before the other log);
`tusb23-excerpt.txt` has only the two keyboard enumerations from it, annotated.

In earlier runs the keyboard was in a higher-numbered port than the gamepad, so every host restart re-created the failing order
and never recovered (usblog15/16/17).

## What I tried
- Did not help: second port reset (as Linux does); slower or paused polling during enumeration; not even opening the gamepad's
  endpoints until the bus had been quiet 2.5 s (no transfer to the pad during the keyboard's enumeration, still fails); 252 MHz;
  self-powered hub; bigger supply capacitor; TinyUSB 0.21.0 and master.
- Moved the failure later in the enumeration:
  - `SIE_CTRL.SOF_SYNC` added for transfers with `need_pre()`: `SET_ADDRESS` and the request for the 18-byte descriptor go
    through (I did not check whether all 18 bytes came back), the configuration descriptor request fails (3 of 3 attempts, `usblog17`).
  - a 300 us wait before each control transaction to a low-speed device behind the hub (master, `usblog19`; in that run my XInput class
    driver was not bound because of the `open()` bug mentioned below, the pad otherwise enumerated): the status stage and
    `SET_ADDRESS` pass; the second packet of the 18-byte descriptor read (sent by the controller itself right after the first) then times out.
- `CFG_TUSB_DEBUG=2` (the build of log 23) does not cure it: it fails in the same way, which is why the log above has the storm.

## Other versions
- 0.18.0 without debug output: the same failure (usblog15, 16, 17), and no recovery by restart because the keyboard was in a higher
  port than the gamepad there. I have not run the lower-port keyboard case on a build without `CFG_TUSB_DEBUG`.
- master `27ed59447`: a receive timeout now fails the transfer at once (no endless retry), TinyUSB retries the control transfer three times
  and then gives up; the keyboard is never retried. In `usblog20` (master, `CFG_TUSB_DEBUG=2`, keyboard alone on the hub, no gamepad
  attached) it fails like this at the 18-byte device descriptor read ("on EP 80 with 16 bytes: FAILED" four times), a case that
  works on 0.18.0. I have not checked whether the debug output affects that.
  (My earlier master/0.21 logs had a gamepad driver bug of my own, the class driver `open()` return type; `usblog20` and later are with it fixed.)

## Observations I can't explain
- Why a device already enumerated on the hub matters, when no endpoint of it is polled during the keyboard's enumeration.
- The status stage and later packets to the keyboard time out once even in the working order; the controller's own retry rescues
  them there (0.18), and not in the failing order. In log 23 the gamepad's 500 ms LED command (an interrupt OUT to the pad) happens to
  start while the keyboard's status stage is pending; the keyboard also fails when the pad's endpoints are closed (above), so I do
  not think it is the cause, but it is in the log.

## Hypothesis (unconfirmed)
Timing at the hub: a packet that follows the previous one too closely is lost when the hub has to switch between the keyboard's
upstream traffic and the downstream preamble traffic. The 300 us experiment supports this (it fixes exactly the steps it covers),
and the controller has no register for the gap between packets of one stage.

## Questions
- Is a low-speed device behind a full-speed hub known to work on RP2040/RP2350, in any order? With which hub?
- Is there a way to add a gap before the preamble packets that I am missing?
- Would an option to enumerate low-speed devices first, or to split control stages into single-packet transactions for
  LS-behind-hub, be acceptable?

## Files
https://github.com/profdc9/tileconsole/tree/main/tilegame/docs/usblogs (`usblog23.bin`, `usblog23-brief.txt`,
`usblog23-annotated.txt`, `tusb23.txt`, `tusb23-excerpt.txt`; also 15, 16, 17 (0.18.0), 18, 19 (master, with my driver bug), 20 (master, fixed)). Decoder: `tilegame/tools/usblog_decode.py` (`--brief` for one line per
request); recorder: `tilegame/board/usblog_usb.c`; notes: `tilegame/docs/usb-and-power.md`.
