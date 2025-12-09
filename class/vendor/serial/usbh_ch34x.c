/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_ch34x.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_ch34x"
#include "usb_log.h"

/* CH34X privateate Data */
struct ch34x_private {
    uint8_t dtr_state;
    uint8_t rts_state;
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static struct ch34x_private g_ch34x_data[CONFIG_USBHOST_MAX_SERIAL_CLASS];

static struct ch34x_private *get_private(struct usbh_serial *serial)
{
    if (serial->minor < CONFIG_USBHOST_MAX_SERIAL_CLASS) {
        return &g_ch34x_data[serial->minor];
    }
    return NULL;
}

static int usbh_ch34x_get_baudrate_div(uint32_t baudrate, uint8_t *factor, uint8_t *divisor)
{
    unsigned char a;
    unsigned char b;
    unsigned long c;

    switch (baudrate) {
        case 921600:

            a = 0xf3;
            b = 7;
            break;

        case 307200:

            a = 0xd9;
            b = 7;
            break;

        default:

            if (baudrate > 6000000 / 255) {
                b = 3;
                c = 6000000;
            } else if (baudrate > 750000 / 255) {
                b = 2;
                c = 750000;
            } else if (baudrate > 93750 / 255) {
                b = 1;
                c = 93750;
            } else {
                b = 0;
                c = 11719;
            }

            a = (unsigned char)(c / baudrate);
            if (a == 0 || a == 0xFF)
                return -USB_ERR_INVAL;
            if ((c / a - baudrate) > (baudrate - c / (a + 1)))
                a++;
            a = 256 - a;
            break;
    }

    *factor = a;
    *divisor = b;
    return 0;
}

static int ch34x_write_handshake(struct usbh_serial *serial, uint8_t dtr, uint8_t rts)
{
    struct usb_setup_packet *setup = serial->hport->setup;

    uint16_t wValue = 0xff;
    if (dtr)
        wValue &= ~(1 << 5);
    if (rts)
        wValue &= ~(1 << 6);

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH341_REQ_MODEM_CTRL;
    setup->wValue = wValue;
    setup->wIndex = 0;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ch34x_set_line_state(struct usbh_serial *serial, bool dtr, bool rts)
{
    struct ch34x_private *private = get_private(serial);

    if (!private)
        return -USB_ERR_INVAL;
    if (!serial || !serial->hport)
        return -USB_ERR_INVAL;

    private->dtr_state = dtr;
    private->rts_state = rts;

    return ch34x_write_handshake(serial, dtr, rts);
}

static int ch34x_set_line_coding(struct usbh_serial *serial, struct cdc_line_coding *line_coding)
{
    struct usb_setup_packet *setup;
    struct ch34x_private *private = get_private(serial);
    uint16_t reg_value = 0;
    uint16_t value = 0;
    uint8_t factor = 0;
    uint8_t divisor = 0;
    int ret;

    if (!serial || !serial->hport || !private)
        return -USB_ERR_INVAL;
    setup = serial->hport->setup;

    reg_value = CH341_LCR_ENABLE_RX | CH341_LCR_ENABLE_TX;

    switch (line_coding->bDataBits) {
        case 5:
            reg_value |= CH341_LCR_CS5;
            break;
        case 6:
            reg_value |= CH341_LCR_CS6;
            break;
        case 7:
            reg_value |= CH341_LCR_CS7;
            break;
        case 8:
            reg_value |= CH341_LCR_CS8;
            break;
        default:
            return -USB_ERR_INVAL;
    }

    if (line_coding->bParityType) {
        reg_value |= CH341_LCR_ENABLE_PAR;
        if (line_coding->bParityType == 2)
            reg_value |= CH341_LCR_PAR_EVEN;
        if (line_coding->bParityType == 3)
            reg_value |= CH341_LCR_MARK_SPACE;
    }

    if (line_coding->bCharFormat == 2) {
        reg_value |= CH341_LCR_STOP_BITS_2;
    }

    value |= 0x9c;
    value |= (reg_value << 8);

    ret = usbh_ch34x_get_baudrate_div(line_coding->dwDTERate, &factor, &divisor);
    if (ret < 0)
        return ret;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH341_REQ_SERIAL_INIT;
    setup->wValue = value;
    setup->wIndex = (factor << 8) | 0x80 | divisor;
    setup->wLength = 0;

    ret = usbh_control_transfer(serial->hport, setup, NULL);
    if (ret < 0)
        return ret;

    return ch34x_write_handshake(serial, private->dtr_state, private->rts_state);
}

static int ch34x_set_flow_control(struct usbh_serial *serial, bool enable)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    uint16_t wIndex = enable ? 0x0101 : 0x0000;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH341_REQ_WRITE_REG;
    setup->wValue = 0x2727;
    setup->wIndex = wIndex;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ch34x_attach(struct usbh_serial *serial)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    struct ch34x_private *private = get_private(serial);
    int ret;
    uint8_t *buffer = serial->io_buf;

    /* Init Private Data */
    if (!private)
        return -USB_ERR_NOMEM;
    memset(private, 0, sizeof(struct ch34x_private));

    /* Read Version */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH341_REQ_READ_VERSION;
    setup->wValue = 0;
    setup->wIndex = 0;
    setup->wLength = 2;

    ret = usbh_control_transfer(serial->hport, setup, buffer);
    if (ret < 0)
        return ret;

    USB_LOG_INFO("Ch34x chip version %02x:%02x\r\n", buffer[0], buffer[1]);

    /* Disable Flow Control */
    ret = ch34x_set_flow_control(serial, false);
    if (ret < 0) {
        USB_LOG_WRN("Ch34x disable flow control failed\r\n");
    }

    /* Set default lines */
    ret = ch34x_write_handshake(serial, false, false);

    return ret;
}

static void ch34x_detach(struct usbh_serial *serial)
{
    struct ch34x_private *private = get_private(serial);
    if (private) {
        memset(private, 0, sizeof(struct ch34x_private));
    }
}

static const struct usbh_serial_driver ch34x_drv = {
    .driver_name = "ch34x",
    .attach = ch34x_attach,
    .detach = ch34x_detach,
    .set_line_coding = ch34x_set_line_coding,
    .set_line_state = ch34x_set_line_state,
    .set_flow_control = ch34x_set_flow_control,
};

static int usbh_ch34x_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = usbh_serial_probe(hport, intf, &ch34x_drv);
    if (serial) {
        hport->config.intf[intf].priv = serial;
        return 0;
    }
    return -USB_ERR_NOMEM;
}

static int usbh_ch34x_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = (struct usbh_serial *)hport->config.intf[intf].priv;
    if (serial) {
        usbh_serial_release(serial);
        hport->config.intf[intf].priv = NULL;
    }
    return 0;
}

static const uint16_t ch34x_id_table[][2] = {
    { 0x1a86, 0x7523 },
    { 0, 0 },
};

const struct usbh_class_driver ch34x_class_driver = {
    .driver_name = "ch34x",
    .connect = usbh_ch34x_connect,
    .disconnect = usbh_ch34x_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info ch34x_class_info = {
    .match_flags = USB_CLASS_MATCH_VID_PID | USB_CLASS_MATCH_INTF_CLASS,
    .bInterfaceClass = 0xff,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
    .id_table = ch34x_id_table,
    .class_driver = &ch34x_class_driver
};