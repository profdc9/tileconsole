# TinyUSB 0.18.0, vendored with one patch (temporary)

Copy of `src/` (only the rp2040 port of `portable/`), `LICENSE`, `version.yml` and `hw/bsp/{family_support.cmake,rp2040}` from TinyUSB
0.18.0, commit `86ad6e56c1700e85f1c5678607a762cfe3aa2f47` (the Pico SDK 2.3.1 submodule).

Patch (`src/portable/raspberrypi/rp2040/hcd_rp2040.c`, `rp2040_usb.c`, `rp2040_usb.h`; marked "tilegame patch"): with
`CFG_TUH_RP2040_LS_GAP_US` set (cmake `-DUSB_LS_GAP_US=300`), each transaction of the control endpoint to a low-speed device behind a
full-speed hub starts that many microseconds after the previous one, and its data stage is sent one packet at a time. Why: see
`../../docs/usb-and-power.md` and `../../docs/tinyusb-issue.md`. To be removed when TinyUSB handles this itself.

Second patch (`hcd_rp2040.c`, `_handle_buff_status_bit`): a buffer status for an endpoint with no active transfer is ignored instead of
panicking in `hw_endpoint_xfer_continue` ("Can't continue xfer on inactive ep 81": usblog25, during the enumeration of a full-speed gamepad
behind a hub). The cause of the late status is not known.

Third patch (`src/host/hub.c`): `tilegame_hub_first_port`, a hub port that `hub_xfer_cb` looks at first when it has a status change, once. The
firmware sets it to the port of the device that failed to enumerate, so that after a host restart that device is enumerated before the others.

Fourth patch (`hcd_rp2040.c`): `tilegame_hcd_spurious` is set to the endpoint address | 0x100 when a buffer status arrives for an endpoint with
no transfer; the firmware then restarts the host (the same condition panicked, and ignoring it silently left the hub's status endpoint dead).

All four are used by `board/usb_hid.c`. Nothing else in the tree is changed.
