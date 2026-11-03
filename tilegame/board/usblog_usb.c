/* What goes in the USB log: the USB interrupt (seen before TinyUSB's own handler acts on it) and every call TinyUSB's host driver makes
 * or receives. The calls are caught with the linker's --wrap, so TinyUSB itself is untouched. */
#include "usb_hid.h"
#include "usblog_board.h"

#include "hardware/irq.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "host/hcd.h"
#include "tusb.h"

/* TinyUSB's RP2040 driver asks the port for its speed on every transfer (to know whether a preamble is needed) and panics ("Invalid speed")
 * if the device has just been unplugged. So nothing is passed on once the port reads as disconnected: it fails, as it would have a moment
 * later, and the removal that is queued cleans up. */
static inline bool port_connected(void)
{
    return usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS;
}

#if USB_PAUSE_POLL
/* While a new device is being enumerated, nothing else is polled. The controller services interrupt endpoints by itself, between the
 * software's control transfers; with a gamepad polling like that, the control transfers to a low-speed device behind a hub (preamble) time
 * out (the USB log: the status stage of its first request, repeating for ever). So a transfer for an interrupt endpoint of a device
 * (not a hub: the hub's status pipe and its ports are part of the enumeration) is held back from an attach until the enumeration is over,
 * and started then. The enumeration is over when TinyUSB reports the new device mounted, when the device is removed again, or after 3 s. */
#define ENUM_MAX_US 3000000u
#define MAX_HELD 12
static volatile uint32_t enum_since_us;   /* when the attach was seen, or 0 */
static struct { uint8_t dev, ep; uint16_t len; uint8_t *buf; bool used; } held[MAX_HELD];

static inline bool enumerating(void)
{
    const uint32_t since = enum_since_us;
    return since && time_us_32() - since < ENUM_MAX_US;
}

void __not_in_flash_func(usblog_usb_attach_seen)(void)
{
    enum_since_us = time_us_32() | 1;
}

void __not_in_flash_func(usblog_usb_enum_over)(void)
{
    enum_since_us = 0;
}

extern bool __real_hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen);

/* Starts what was held back, once the enumeration is over (from the USB task). */
void usblog_usb_release_held(void)
{
    if (enumerating())
        return;
    for (int i = 0; i < MAX_HELD; i++)
        if (held[i].used) {
            held[i].used = false;
            const bool ok = port_connected() && __real_hcd_edpt_xfer(0, held[i].dev, held[i].ep, held[i].buf, held[i].len);
            usblog(USBLOG_EV_XFER, (uint16_t)(held[i].dev << 8 | held[i].ep), held[i].len, ok, 1, 0);   /* (c = 1: it had been held) */
        }
}

/* Nothing held may start on a device that is gone. */
static void drop_held(uint8_t dev_addr)
{
    for (int i = 0; i < MAX_HELD; i++)
        if (held[i].used && held[i].dev == dev_addr)
            held[i].used = false;
}

#endif

/* TinyUSB calls this when a device's enumeration is complete, whatever its class. */
void tuh_mount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
#if USB_PAUSE_POLL
    usblog_usb_enum_over();
#endif
    usb_hid_host_mounted();
}

/* TinyUSB masks the USB interrupt around its event queue (usbh_int_set). The USB log of a hang shows the hub's status endpoint finished and the
 * interrupt never serviced, while the program kept running: so who turned it off, and when, is kept for the hang record. */
static volatile uint32_t irq_off_us, irq_off_caller, irq_on_us;
static volatile bool irq_off;

extern void __real_hcd_int_disable(uint8_t rhport);
void __wrap_hcd_int_disable(uint8_t rhport)
{
    irq_off_caller = (uint32_t)__builtin_return_address(0);
    irq_off_us = time_us_32();
    irq_off = true;
    __real_hcd_int_disable(rhport);
}

extern void __real_hcd_int_enable(uint8_t rhport);
void __wrap_hcd_int_enable(uint8_t rhport)
{
    irq_off = false;
    irq_on_us = time_us_32();
    __real_hcd_int_enable(rhport);
}

void __not_in_flash_func(usblog_usb_irq_state)(void)
{
    usblog(USBLOG_EV_IRQ_STATE, irq_off, irq_off_us, irq_off_caller, irq_on_us, 0);
}

/* An interrupt that reads exactly as the last one (a storm of receive timeouts, every 100 microseconds) is counted, not recorded each
 * time: the ring would hold only the storm, and not how it started. The count goes in when something different comes, and every 4096. */
static struct { uint32_t ints, sie_status, buf_status, epx, ctrl; uint32_t repeats, first_us, last_us, run_us; bool have, storm; } last_isr;

void __not_in_flash_func(usblog_usb_flush)(void)
{
    if (last_isr.repeats) {
        usblog(USBLOG_EV_ISR_REPEAT, 0, last_isr.repeats, last_isr.last_us - last_isr.first_us, 0, 0);
        last_isr.repeats = 0;
    }
}

/* A control transfer the controller keeps retrying (the device behind a hub does not answer) gives a receive timeout every ~100 us until the
 * device is unplugged. Other devices' polling interrupts come in between, so identical interrupts in a row are not a way to see it: count the
 * timeouts since a control transfer last succeeded. About 3000 of them is a third of a second of nothing. */
#define STORM_TIMEOUTS 3000u
static uint32_t timeout_run;

/* TinyUSB's handler is added with the highest priority; one added later with the same priority runs before it, and only reads. */
static void __not_in_flash_func(observe_irq)(void)
{
    const uint32_t ints = usb_hw->ints, sie_status = usb_hw->sie_status, buf_status = usb_hw->buf_status, epx = usbh_dpram->epx_buf_ctrl;
    const uint32_t ctrl = usb_hw->sie_ctrl & 0xFFFF;
    const uint32_t now = time_us_32();
    if (ints & USB_INTS_ERROR_RX_TIMEOUT_BITS) {
        if (++timeout_run == STORM_TIMEOUTS)
            usb_hid_host_storm();
    } else if ((ints & USB_INTS_TRANS_COMPLETE_BITS) || (buf_status & 1u)) {
        timeout_run = 0;   /* (buffer 0 is the control endpoint's) */
    }
    if (last_isr.have && ints == last_isr.ints && sie_status == last_isr.sie_status && buf_status == last_isr.buf_status && epx == last_isr.epx &&
        ctrl == last_isr.ctrl) {
        if (!last_isr.repeats)
            last_isr.first_us = now;
        last_isr.last_us = now;
        if (!last_isr.storm && (ints & USB_INTS_ERROR_RX_TIMEOUT_BITS) && now - last_isr.run_us > 1000000u) {
            last_isr.storm = true;   /* the same receive timeout for a second: the host has to start again */
            usb_hid_host_storm();
        }
        if (++last_isr.repeats >= 4096)
            usblog_usb_flush();
        return;
    }
    usblog_usb_flush();
    last_isr.ints = ints;
    last_isr.sie_status = sie_status;
    last_isr.buf_status = buf_status;
    last_isr.epx = epx;
    last_isr.ctrl = ctrl;
    last_isr.have = true;
    last_isr.run_us = now;
    last_isr.storm = false;
    usblog(USBLOG_EV_ISR, (uint16_t)ctrl, ints, sie_status, buf_status, epx);
}

void usblog_usb_init(void)
{
    irq_add_shared_handler(USBCTRL_IRQ, observe_irq, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
}

void usblog_usb_remove(void)
{
    irq_remove_handler(USBCTRL_IRQ, observe_irq);
    last_isr.have = false;
    last_isr.repeats = 0;
}

/* Attach, remove and transfer complete: these come from the interrupt handler */
extern void __real_hcd_event_handler(hcd_event_t const *event, bool in_isr);
void __not_in_flash_func(__wrap_hcd_event_handler)(hcd_event_t const *event, bool in_isr)
{
    uint32_t a = 0, b = 0;
    if (event->event_id == HCD_EVENT_XFER_COMPLETE) {
        if ((event->xfer_complete.ep_addr & 0x7F) == 0)
            usb_hid_host_ctrl_done();
        a = event->xfer_complete.ep_addr | event->xfer_complete.result << 8;
        b = event->xfer_complete.len;
    } else if (event->event_id == HCD_EVENT_DEVICE_ATTACH || event->event_id == HCD_EVENT_DEVICE_REMOVE) {
        a = event->connection.hub_addr | event->connection.hub_port << 8;
#if TUSB_VERSION_NUMBER < 2100
        a |= event->connection.speed << 16;   /* (TinyUSB 0.21 no longer carries it) */
#endif
    }
    usblog(USBLOG_EV_HCD_EVENT, (uint16_t)(event->event_id << 8 | event->dev_addr), a, b, in_isr, 0);
    if (event->event_id == HCD_EVENT_DEVICE_ATTACH || event->event_id == HCD_EVENT_DEVICE_REMOVE)
        usb_hid_host_event(event->event_id, event->connection.hub_addr, event->connection.hub_port);
#if USB_PAUSE_POLL
    if (event->event_id == HCD_EVENT_DEVICE_ATTACH)
        usblog_usb_attach_seen();
    else if (event->event_id == HCD_EVENT_DEVICE_REMOVE)
        usblog_usb_enum_over();   /* (the device that was being enumerated may be the one that went) */
#endif
    __real_hcd_event_handler(event, in_isr);
}

extern bool __real_hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen);
/* USB_PRE_DELAY_US: wait this long before a control transaction to a low-speed device behind a full-speed hub (preamble), after the
 * previous one ended (an experiment: the status stage after the descriptor has been lost, with no answer, in every enumeration). */
#if USB_PRE_DELAY_US
static void pre_delay(uint8_t dev_addr)
{
    if (tuh_speed_get(dev_addr) == TUSB_SPEED_LOW && (usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS) == (2u << USB_SIE_STATUS_SPEED_LSB))
        busy_wait_us_32(USB_PRE_DELAY_US);
}
#else
#define pre_delay(a) ((void)0)
#endif

bool __wrap_hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen)
{
#if USB_PAUSE_POLL
    if ((ep_addr & 0x7F) != 0 && dev_addr <= CFG_TUH_DEVICE_MAX && enumerating()) {   /* an interrupt endpoint of a device, not a hub's */
        for (int i = 0; i < MAX_HELD; i++)
            if (!held[i].used) {
                held[i].dev = dev_addr;
                held[i].ep = ep_addr;
                held[i].len = buflen;
                held[i].buf = buffer;
                held[i].used = true;
                usblog(USBLOG_EV_XFER, (uint16_t)(dev_addr << 8 | ep_addr), buflen, 1, 2, 0);   /* (c = 2: held back) */
                return true;
            }
    }
#endif
    if ((ep_addr & 0x7F) == 0) {
        pre_delay(dev_addr);
        usb_hid_host_ctrl_start();
    }
    const bool ok = port_connected() && __real_hcd_edpt_xfer(rhport, dev_addr, ep_addr, buffer, buflen);
    usblog(USBLOG_EV_XFER, (uint16_t)(dev_addr << 8 | ep_addr), buflen, ok, 0, 0);
    return ok;
}

extern bool __real_hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8]);
bool __wrap_hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8])
{
    pre_delay(dev_addr);
    usb_hid_host_ctrl_start();
    const bool ok = port_connected() && __real_hcd_setup_send(rhport, dev_addr, setup_packet);
    if (dev_addr == 0)
        usb_hid_host_setup0();   /* a new device is being enumerated: the attach has gone somewhere */
    uint32_t w[2];
    memcpy(w, setup_packet, 8);
    usblog(USBLOG_EV_SETUP, dev_addr, w[0], w[1], ok, 0);
    return ok;
}

extern bool __real_hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *ep_desc);
bool __wrap_hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *ep_desc)
{
#if USB_FS_INT_MS
    /* The controller polls interrupt endpoints by itself, between the software's control transfers. A full-speed gamepad polled every
     * millisecond leaves a low-speed device behind a hub (preamble, 1.5 Mb/s) no room to enumerate: its control transfer times out. A
     * longer interval for full-speed interrupt endpoints leaves room. */
    tusb_desc_endpoint_t slowed;
    if (ep_desc->bmAttributes.xfer == TUSB_XFER_INTERRUPT && ep_desc->bInterval < USB_FS_INT_MS && tuh_speed_get(dev_addr) == TUSB_SPEED_FULL) {
        slowed = *ep_desc;
        slowed.bInterval = USB_FS_INT_MS;
        ep_desc = &slowed;
    }
#endif
    const bool ok = port_connected() && __real_hcd_edpt_open(rhport, dev_addr, ep_desc);
    usblog(USBLOG_EV_EP_OPEN, (uint16_t)(dev_addr << 8 | ep_desc->bEndpointAddress), ep_desc->wMaxPacketSize, ep_desc->bmAttributes.xfer | ep_desc->bInterval << 8, ok, 0);
    return ok;
}

extern void __real_hcd_port_reset(uint8_t rhport);
void __wrap_hcd_port_reset(uint8_t rhport)
{
    usblog(USBLOG_EV_PORT, 0, 1, 0, 0, 0);
    __real_hcd_port_reset(rhport);
}

extern void __real_hcd_port_reset_end(uint8_t rhport);
void __wrap_hcd_port_reset_end(uint8_t rhport)
{
    usblog(USBLOG_EV_PORT, 0, 2, 0, 0, 0);
    __real_hcd_port_reset_end(rhport);
}

extern void __real_hcd_device_close(uint8_t rhport, uint8_t dev_addr);
void __wrap_hcd_device_close(uint8_t rhport, uint8_t dev_addr)
{
    usblog(USBLOG_EV_PORT, dev_addr, 3, 0, 0, 0);
#if USB_PAUSE_POLL
    drop_held(dev_addr);
#endif
    __real_hcd_device_close(rhport, dev_addr);
}

extern bool __real_hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr);
bool __wrap_hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr)
{
    usblog(USBLOG_EV_PORT, (uint16_t)(dev_addr << 8 | ep_addr), 4, 0, 0, 0);
    return __real_hcd_edpt_abort_xfer(rhport, dev_addr, ep_addr);
}

extern bool __real_hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr);
bool __wrap_hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr)
{
    usblog(USBLOG_EV_PORT, (uint16_t)(dev_addr << 8 | ep_addr), 5, 0, 0, 0);
    return __real_hcd_edpt_clear_stall(rhport, dev_addr, ep_addr);
}
