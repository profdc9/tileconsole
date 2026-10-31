/* SPDX-License-Identifier: MIT */
/*
 * An XInput (Xbox 360 style) wired gamepad, as a TinyUSB host class driver. XInput is not HID: the pad shows up as a
 * vendor-specific interface (class 0xFF, subclass 0x5D, protocol 1) with an interrupt IN endpoint that sends 20-byte input
 * reports, and an OUT endpoint for rumble and LEDs, which are not used here. A wired pad needs no start-up handshake: it
 * sends reports as soon as the endpoint is polled. The decoding is in xinput.c; this file only moves the bytes.
 */
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "tusb.h"
#include "host/usbh_pvt.h"

#include "usb_hid.h"

typedef struct {
    bool used;
    uint8_t ep_in, ep_out;
    uint8_t in_interval, out_interval;
    uint16_t in_mps;
    uint32_t reports;      /* reports with data: until there is one, the LED command is repeated */
    uint32_t last_led_ms;
    uint8_t buf[32] TU_ATTR_ALIGNED(4);
    uint8_t out[8] TU_ATTR_ALIGNED(4);   /* what is sent on the OUT endpoint */
} xdev_t;

static xdev_t xdev[CFG_TUH_DEVICE_MAX + 1];

/* Counters for the diagnostic screen: where does the traffic stop? */
static struct {
    uint32_t opened, configured, queued, claim_failed, xfer_failed, callbacks, callbacks_ok;
    uint32_t last_result, last_len;
    uint32_t out_queued, out_done, out_result, out_failed;
} dbg;

static bool x_init(void)
{
    memset(xdev, 0, sizeof xdev);
    return true;
}

static bool x_deinit(void) { return true; }

static void start_receive(uint8_t dev_addr, xdev_t *x)
{
    if (!usbh_edpt_claim(dev_addr, x->ep_in)) {
        dbg.claim_failed++;
        return;
    }
    if (usbh_edpt_xfer(dev_addr, x->ep_in, x->buf, sizeof x->buf)) {
        dbg.queued++;
    } else {
        dbg.xfer_failed++;
        usbh_edpt_release(dev_addr, x->ep_in);
    }
}

static void send_led(uint8_t dev_addr, xdev_t *x)
{
    if (!x->ep_out)
        return;
    x->out[0] = 0x01;
    x->out[1] = 0x03;
    x->out[2] = 0x06;
    x->last_led_ms = to_ms_since_boot(get_absolute_time());
    if (!usbh_edpt_claim(dev_addr, x->ep_out)) {
        dbg.out_failed++;
        return;
    }
    if (usbh_edpt_xfer(dev_addr, x->ep_out, x->out, 3)) {
        dbg.out_queued++;
    } else {
        dbg.out_failed++;
        usbh_edpt_release(dev_addr, x->ep_out);
    }
}

/* A pad that has not sent a report with data yet is asked again, twice a second. */
void xinput_poll(void)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    for (uint8_t d = 1; d <= CFG_TUH_DEVICE_MAX; d++) {
        xdev_t *x = &xdev[d];
        if (x->used && x->reports == 0 && now - x->last_led_ms >= 500)
            send_led(d, x);
    }
}

static bool x_open(uint8_t rhport, uint8_t dev_addr, const tusb_desc_interface_t *itf, uint16_t max_len)
{
    (void)rhport;
    if (itf->bInterfaceClass != 0xFF || itf->bInterfaceSubClass != 0x5D || itf->bInterfaceProtocol != 0x01)
        return false;   /* not a pad: headsets and other XInput peripherals use other protocols */
    if (dev_addr > CFG_TUH_DEVICE_MAX || xdev[dev_addr].used)
        return false;
    xdev_t *x = &xdev[dev_addr];
    memset(x, 0, sizeof *x);

    const uint8_t *p = (const uint8_t *)itf, *end = p + max_len;
    for (p = tu_desc_next(p); p < end && tu_desc_len(p) != 0; p = tu_desc_next(p)) {
        if (tu_desc_type(p) == TUSB_DESC_INTERFACE)
            break;   /* the next interface */
        if (tu_desc_type(p) != TUSB_DESC_ENDPOINT)
            continue;   /* the class-specific descriptor in between */
        const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
        if (ep->bmAttributes.xfer != TUSB_XFER_INTERRUPT)
            continue;
        if (!tuh_edpt_open(dev_addr, ep))
            return false;
        if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
            if (!x->ep_in) {
                x->ep_in = ep->bEndpointAddress;
                x->in_mps = tu_edpt_packet_size(ep);
                x->in_interval = ep->bInterval;
            }
        } else if (!x->ep_out) {
            x->ep_out = ep->bEndpointAddress;
            x->out_interval = ep->bInterval;
        }
    }
    if (!x->ep_in)
        return false;
    x->used = true;
    dbg.opened++;
    usb_hid_xinput_attach(dev_addr);
    return true;
}

static bool x_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    xdev_t *x = &xdev[dev_addr];
    dbg.configured++;
    if (x->used) {
        start_receive(dev_addr, x);
        /* What Linux's xpad driver does for every Xbox 360 style pad: set the LED ("player 1 on"). Some third-party pads do not
         * send input until the host has said something, and a pad that does not care ignores it. */
        send_led(dev_addr, x);
    }
    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool x_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    xdev_t *x = &xdev[dev_addr];
    if (x->used && ep_addr == x->ep_out) {   /* the LED command went out */
        dbg.out_done++;
        dbg.out_result = (uint32_t)result;
        return true;
    }
    if (!x->used || ep_addr != x->ep_in)
        return true;
    dbg.callbacks++;
    dbg.last_result = (uint32_t)result;
    dbg.last_len = xferred_bytes;
    if (result == XFER_RESULT_SUCCESS) {
        dbg.callbacks_ok++;
        if (xferred_bytes)
            x->reports++;
        usb_hid_xinput_report(dev_addr, x->buf, (uint16_t)xferred_bytes);
        start_receive(dev_addr, x);   /* ask for the next report */
    }
    return true;
}

static void x_close(uint8_t dev_addr)
{
    if (dev_addr > CFG_TUH_DEVICE_MAX)
        return;
    if (xdev[dev_addr].used)
        usb_hid_xinput_detach(dev_addr);
    memset(&xdev[dev_addr], 0, sizeof xdev[dev_addr]);
}

static const usbh_class_driver_t xinput_driver = {
    .name = "XINPUT", .init = x_init, .deinit = x_deinit, .open = x_open, .set_config = x_set_config, .xfer_cb = x_xfer_cb,
    .close = x_close,
};

const usbh_class_driver_t *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &xinput_driver;
}

int xinput_debug(char (*lines)[41], int max_lines)
{
    int n = 0;
#define LINE(...) do { if (n < max_lines) snprintf(lines[n++], 41, __VA_ARGS__); } while (0)
    LINE("XD OPEN %lu CFG %lu TOTAL QUEUED %lu", (unsigned long)dbg.opened, (unsigned long)dbg.configured, (unsigned long)dbg.queued);
    LINE("XD CLAIMFAIL %lu XFERFAIL %lu", (unsigned long)dbg.claim_failed, (unsigned long)dbg.xfer_failed);
    LINE("XD CALLBACKS %lu OK %lu", (unsigned long)dbg.callbacks, (unsigned long)dbg.callbacks_ok);
    LINE("XD LAST RESULT %lu LENGTH %lu", (unsigned long)dbg.last_result, (unsigned long)dbg.last_len);
    LINE("XD LED QUEUED %lu DONE %lu RES %lu FAIL %lu", (unsigned long)dbg.out_queued, (unsigned long)dbg.out_done,
         (unsigned long)dbg.out_result, (unsigned long)dbg.out_failed);
    for (uint8_t d = 1; d <= CFG_TUH_DEVICE_MAX; d++) {
        const xdev_t *x = &xdev[d];
        if (x->used) {
            LINE("XD DEV %u SPEED %d REPORTS %lu", d, (int)tuh_speed_get(d), (unsigned long)x->reports);
            LINE("XD IN %02X MPS %u IVL %u OUT %02X IVL %u", x->ep_in, x->in_mps, x->in_interval, x->ep_out, x->out_interval);
        }
    }
#undef LINE
    return n;
}
