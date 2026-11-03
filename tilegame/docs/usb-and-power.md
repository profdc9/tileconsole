# USB host and power: what was found

Findings from debugging the USB host on the EmuBoard2350 (RP2350B, TinyUSB 0.18.0 from Pico SDK 2.3.1). The code is in
`board/usb_hid.c`, `board/usblog_usb.c`, `board/usblog_board.c` and `src/usblog.c`.

## The USB event log

`-DUSBLOG=ON` (the default) keeps a ring of events in RAM that survives a watchdog reset: control and interrupt transfers,
controller interrupts (status registers, with runs of identical records collapsed), TinyUSB's device events, host restarts, panics
and fault registers. The devkit saves it to the SD card or USB stick:

- `USBLOG` in BASIC writes `usblogNN.bin`; after a panic or reset the board writes one by itself.
- `tools/usblog_decode.py usblogNN.bin` prints it as text (`ISR`, `setup`, `event ATTACH/REMOVE`, `USB HOST RESTARTED ...`).
- `tests/test_usblog.c` tests the ring and the decoder format (`make check`).

Capturing what Linux does with the same devices (`usbmon`, Wireshark) was the way to see what the board should have done.

## Problems in TinyUSB 0.18.0 (fixed upstream in 0.21.0 or worked around here)

- **Enumeration never forgotten.** A device that goes away during its enumeration leaves the stack waiting for it, so every later
  attach is deferred behind it, and two queued attaches make `tuh_task` loop for ever (the hang). Recovery: the host is restarted
  (`tuh_deinit`, `tuh_init`) 300 ms after a root port unplug, or when an attach does not reach a setup packet to address 0
  within 2 s.
- **Panic on speed.** `hcd_port_speed_get` panics ("Invalid speed") when called after the unplug. Wrapped (`--wrap`) to say "full".
- **Receive timeout storm.** A transfer to a device that stops answering (here: the keyboard's `SET_ADDRESS` behind the hub) is
  retried by the controller for ever, thousands of timeouts per second, with no way out for the driver. Detected as 3000 receive
  timeouts without a completed control transfer, then the host is restarted. At most 2 such restarts in a row
  (`AUTO_RESTARTS_MAX`); a device that fails the same way each time is not helped by more, and the loop made the gamepad drop
  every 2.5 s.
- **Control transfers in mount callbacks, data sequence panic:** see the README.
- Experiments that did not help and are off by default: `USB_PAUSE_POLL` (hold other devices' polling during an enumeration), `USB_PAD_LATE` (open and poll the pad only after the bus has been quiet),
  `USB_FS_INT_MS` (slower polling for full-speed endpoints), and a second port reset (patched TinyUSB, as Linux does).

## The keyboard behind the hub

Observed with a bus-powered USB 3.0 hub, a low-speed keyboard and a full-speed XInput gamepad:

- Keyboard and hub from the start, then the gamepad: both work.
- Gamepad first, then the keyboard (or the keyboard replugged while the gamepad runs): the keyboard's first descriptor read works,
  `SET_ADDRESS` gets only receive timeouts. After a host restart the order is gamepad first again, so it fails again.
- Every successful enumeration in the logs followed a disconnect/connect flap of the port.
- Linux enumerates the same keyboard through the hub's transaction translator, with a second port reset; the board talks to the
  hub at full speed and uses preamble packets for the low-speed device.

Workaround: plug the keyboard in before the gamepad.

It is not the supply: with a self-powered hub the keyboard fails to enumerate after a replug in the same way, and the failure
stayed when the board's 5 V rail was made stiffer (below).

It is not the gamepad's polling either. With `USB_PAD_LATE` the pad's endpoints are not even opened until the bus has been quiet
for 2.5 s, and the log (usblog16) shows no transfer to any endpoint of the pad while the keyboard is enumerated. The keyboard still
fails: the first 8 bytes of its device descriptor are read, then the zero-length status stage gets receive timeouts, thousands a
second, until the host restart. What matters is only that the pad has already been enumerated (addressed and configured) on the
hub; with the keyboard enumerated first, everything works. The fault is in the control transfer to the low-speed device through
the hub (preamble packets, the hub driven at full speed). Not found: Linux, which uses the hub's transaction translator and resets
the port a second time, does not hit it.
A further test set the controller's `SOF_SYNC` bit (packets start after the frame marker) on control transfers to a low-speed
device behind the hub (a one-line change to `hcd_rp2040.c` in a copy of TinyUSB; usblog17). It changed where the keyboard fails,
the same way each time: its device descriptor, `SET_ADDRESS` and the 18-byte descriptor are now read (each SETUP needing 2 to 5
retries, one a frame), and the request for the configuration descriptor then gets timeouts until the host restart. Not enough.
Left in the code as default-off experiments: `USB_PAD_LATE`, `USB_PAUSE_POLL`, `USB_FS_INT_MS`.

## Power

The board runs from the barrel jack (11.65 V in this setup): D2, a KA78M05 (500 mA), D3 (SS14), then the 5 V rail that feeds the
AMS1117-3.3, the USB-A port (VBUS) and the Mini-DINs.

- Plugging in a bus-powered hub makes the supply dip (seen on a scope; the hub's VBUS was 4.65 V). At 504 MHz the core runs at
  1.65 V and draws much more, so the board restarted silently when the hub went in (no "Last run" line) or just after a game
  started. At **252 MHz** the board stays up with the hub; this is the `CPU_MHZ=252` build option (the default stays 504 MHz).
- **Fixed by a bulk capacitor:** replacing the 220 uF capacitor with 1000 uF (5 V rail) lets a bus-powered hub be plugged in and
  unplugged at 504 MHz without the board restarting. The keyboard enumeration failure is unchanged by it.
- The dip does not appear when the RP2350 is held in reset, so the hub's load is what the regulator cannot supply on top.
- **USB-C trap:** the USB-C connector's D+/D- are tied to the USB-A port's D+/D-. A charger that shorts D+ to D- (for charging
  detection) breaks the host port, so devices stopped working with a USB-C charger.
- Hardware fixes (the capacitor above was the one that mattered for restarts), in order of effort: a self-powered hub; a lower barrel voltage (less dissipation in the KA78M05); a bulk
  capacitor next to J11; a switching regulator and a current-limited USB switch for VBUS; not powering from a USB-C charger.
