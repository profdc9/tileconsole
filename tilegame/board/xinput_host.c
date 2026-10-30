/* SPDX-License-Identifier: MIT */
/*
 * An XInput (Xbox 360 style) wired gamepad, as a TinyUSB host class driver. XInput is not HID: the pad shows up as a
 * vendor-specific interface (class 0xFF, subclass 0x5D, protocol 1) with an interrupt IN endpoint that sends 20-byte input
 * reports, and an OUT endpoint for rumble and LEDs, which are not used here. A wired pad needs no start-up handshake: it
 * sends reports as soon as the endpoint is polled. The decoding is in xinput.c; this file only moves the bytes.
 */
#include <string.h>

#include "tusb.h"
#include "host/usbh_pvt.h"

#include "usb_hid.h"

typedef struct {
    bool used;
    uint8_t ep_in, ep_out;
    uint8_t buf[32] TU_ATTR_ALIGNED(4);
} xdev_t;

static xdev_t xdev[CFG_TUH_DEVICE_MAX + 1];

static bool x_init(void)
{
    memset(xdev, 0, sizeof xdev);
    return true;
}

static bool x_deinit(void) { return true; }

static void start_receive(uint8_t dev_addr, xdev_t *x)
{
    if (usbh_edpt_claim(dev_addr, x->ep_in) && !usbh_edpt_xfer(dev_addr, x->ep_in, x->buf, sizeof x->buf))
        usbh_edpt_release(dev_addr, x->ep_in);
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
            if (!x->ep_in)
                x->ep_in = ep->bEndpointAddress;
        } else if (!x->ep_out) {
            x->ep_out = ep->bEndpointAddress;
        }
    }
    if (!x->ep_in)
        return false;
    x->used = true;
    usb_hid_xinput_attach(dev_addr);
    return true;
}

static bool x_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    xdev_t *x = &xdev[dev_addr];
    if (x->used)
        start_receive(dev_addr, x);
    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool x_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    xdev_t *x = &xdev[dev_addr];
    if (!x->used || ep_addr != x->ep_in)
        return true;   /* the OUT endpoint is not used */
    if (result == XFER_RESULT_SUCCESS) {
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
