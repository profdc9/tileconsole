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
TinyUSB master (27ed59447, Oct 2026) does not fix it either (usblog18): a receive timeout now fails the transfer at once instead of
being retried for ever by the controller, so there is no storm, but the status stage after the keyboard's first descriptor read
still times out and nothing retries the keyboard.

What did move the failure is time between packets (`USB_PRE_DELAY_US=300`, usblog19, on master): waiting 300 us before each
control transaction to the low-speed device makes the status stage and `SET_ADDRESS` succeed, where they failed without it. The next
failure is the second packet of a multi-packet data stage (the 18-byte device descriptor, 8-byte packets): the controller sends the
next IN token by itself right after the first packet, and that one times out. So the suspect is the turnaround at the hub: a
transaction that starts too soon after the hub has relayed the keyboard's upstream packet is lost, and the RP2040/RP2350 controller
has no register for the gap, and starts packets inside a stage on its own. Only a driver that issues each packet itself, with its own
data toggle handling (not just a wrapper around TinyUSB's `hcd_edpt_xfer`), could add the gap. Not done.
Left in the code as default-off experiments (`USB_PRE_DELAY_US` too): `USB_PAD_LATE`, `USB_PAUSE_POLL`, `USB_FS_INT_MS`.

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

## The workaround in the firmware (vendored TinyUSB 0.18.0, `third_party/tinyusb`)

What the logs showed (usblog23 to 31): a low-speed keyboard behind the hub fails (control requests lost, the controller retries for
ever) when it is enumerated after a full-speed gamepad on the same hub, and works when it is enumerated first. A 300 us gap between
transactions (`USB_LS_GAP_US`, one packet at a time) did not fix the failure (usblog27: the third packet of the descriptor read is
lost, with a data sequence error, probably the controller's own ACK to the keyboard), so it is not what the workaround relies on.

The workaround is to recover, and to make the recovery enumerate the keyboard first:
- the receive timeout storm detector restarts the USB host after about 0.3 s (`usb_hid.c`, at most 2 automatic restarts in a row);
- the hub port where the storm hit is remembered, and after a restart the vendored hub driver looks at that port first
  (`tilegame_hub_first_port` in `third_party/tinyusb/src/host/hub.c`), whatever its number, so the keyboard is enumerated before the gamepad;
- a control transfer that has not completed for 2 s also restarts the host (TinyUSB 0.18 has no timeout; the stall was seen on a gamepad's
  HID report descriptor read, usblog26);
- a buffer status for an endpoint with no transfer is ignored instead of panicking (usblog25).

Result: keyboard plugged into the hub after the gamepad (usblog30, keyboard in hub port 4, gamepad in port 3): the keyboard fails once,
the host restarts after 0.8 s, the keyboard comes up first and then the gamepad (both working about 2 s after the plug-in). Everything
plugged in at power-up (usblog31): no restarts. The cause of the failure is still not known; the TinyUSB issue draft
(`tinyusb-issue.md`) describes it.
