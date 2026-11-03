# rp2040/hcd: low-speed keyboard behind a full-speed-driven hub fails to enumerate when another device is already enumerated

(Draft for hathach/tinyusb. Related: #4180 lists "low-speed device behind a full-speed hub (PREAMBLE)" as not validated.)

## Setup
- Board: RP2350B (custom "EmuBoard2350"), USB host on the native port, `BOARD_TUH_RHPORT 0`, `CFG_TUH_ENABLED 1`, OS none.
- TinyUSB: tested 0.18.0 (Pico SDK 2.3.1), 0.21.0, and master `27ed59447` (0.21.0-798). Same symptom in all three.
- Devices: a bus-powered USB 3.0 hub (also tried a self-powered hub and a third hub; all behave the same), a full-speed
  XInput gamepad (composite: two HID interfaces plus a vendor XInput interface) and a low-speed HID keyboard, both on the hub.
- The RP2350 host runs at full speed, so the hub is used in full-speed mode: the keyboard is reached with preamble
  (`SIE_CTRL.PREAMBLE_EN`, `INTEP_PREAMBLE`). Linux, on a high-speed host, enumerates the same keyboard through the hub's TT with no trouble.
- System clock 504 MHz (also tried 252 MHz), 5 V rail checked on a scope; a larger bulk capacitor fixed brown-outs on hub insertion
  but did not change this.

## Symptom
- Hub + keyboard plugged in first, gamepad added later: everything works.
- Hub + gamepad enumerated first, then the keyboard plugged into the hub (or replugged): the keyboard never enumerates.
  After a failed attempt it stays dead until the hub is unplugged. This is 100% reproducible with all three hubs.
- Every enumeration of the keyboard that works has a first status-stage packet that also times out once and then succeeds on the
  controller's automatic retry. When it fails, the retries never succeed.

## What the bus does (RAM log of SETUP/xfer calls and USB ISR registers, decoded)
Keyboard on hub port 4, root port full speed, `sie_ctrl` with `PREAMBLE_EN`:
```
setup dev 0: IN GET_DESCRIPTOR DEVICE wLength 8      ISR TRANS_COMPLETE            (setup + data OK, 8 bytes read)
transfer start dev 0 ep 0 OUT 0 bytes                (status stage, DATA1, sie_ctrl=SEND_DATA|PREAMBLE_EN)
ISR ints=ERROR_RX_TIMEOUT  epx_buf_ctrl=0000F400     <- no answer, 72 us after the transfer started
```
- 0.18.0 / 0.21.0: the controller retries the RX timeout forever (thousands per second) or until the stack gives up; the hub's own
  control requests also stop being answered during that time. With master the transfer fails at once (`XFER_COMPLETE ... FAILED`),
  no storm, but the keyboard is not retried and stays unenumerated.
- Sometimes the status stage passes and the failure is at `SET_ADDRESS` or at the next control read (observed in different runs).
- `sie_ctrl`, `epx_buf_ctrl` and the PIDs (DATA1 status stage) are identical in the working and the failing runs.
- While the keyboard is enumerated none of the gamepad's endpoints is being polled (I delayed opening them to check), so it is not
  interference from hardware-polled interrupt endpoints, and not power.

## Experiments that change where it fails
1. `SIE_CTRL.SOF_SYNC` added for transfers with `need_pre()` (one line in `hcd_edpt_xfer` and `hcd_setup_send`): the keyboard now
   gets through `SET_ADDRESS` and the 18-byte device descriptor (each SETUP needing 2-5 retries, one per frame) and fails at the
   configuration descriptor request (9 bytes = two packets), the same way in 3 of 3 attempts.
2. Waiting 300 us in a `--wrap` of `hcd_edpt_xfer`/`hcd_setup_send` before each control transaction to a low-speed device behind
   the hub (master): the status stage and `SET_ADDRESS` now succeed. The next failure is the second packet of the 18-byte device
   descriptor read: the first 8-byte packet arrives, then the second IN token, sent by the controller itself right after, times out.
3. Did not help: second port reset after the first (as Linux does), stretched/paused polling, 252 MHz, a self-powered hub, a
   bigger supply capacitor, TinyUSB 0.21 and master.

## Hypothesis (not confirmed)
A transaction that starts too soon after the hub has relayed the keyboard's upstream packet (or the host's handshake) is lost
when the hub has to turn around between upstream and downstream (preamble) traffic. Software steps can be spaced out (experiment
2), but packets inside one stage are sent by the SIE back to back and there is no register for the gap. Why it depends on the
gamepad already being enumerated on the same hub is not understood (only the keyboard-first order works).

## Questions
- Is a low-speed device behind a full-speed hub known to work on RP2040/RP2350 with some hub? Which?
- Is there a way to add an inter-packet gap or a preamble-to-packet delay (anything in `SIE_CTRL` / `ADDR_ENDP` I am missing)?
- Would the maintainers accept a driver option that splits control stages into single-packet transactions for LS-behind-hub?

Decoded logs (`usblog18`, `usblog19`) and the tool that records and decodes them are in https://github.com/profdc9/tileconsole
(`tilegame/board/usblog_usb.c`, `tilegame/tools/usblog_decode.py`, notes in `tilegame/docs/usb-and-power.md`). I can attach the
raw logs.
