/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_ch34x.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_ch34x"
#include "usb_log.h"

static int usbh_ch34x_get_baudrate_div(uint32_t baudrate, uint8_t *factor, uint8_t *divisor)
{
    uint8_t a;
    uint8_t b;
    uint32_t c;

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
            a = (uint8_t)(c / baudrate);
            if (a == 0 || a == 0xFF) {
                return -USB_ERR_INVAL;
            }
            if ((c / a - baudrate) > (baudrate - c / (a + 1))) {
                a++;
            }
            a = (uint8_t)(256 - a);
            break;
    }

    *factor = a;
    *divisor = b;

    return 0;
}

static int ch34x_set_line_coding(struct usbh_serial *serial, struct cdc_line_coding *line_coding)
{
    struct usb_setup_packet *setup;
    struct ch34x_priv *priv = (struct ch34x_priv *)serial->priv;
    uint16_t reg_value = 0;
    uint16_t value = 0;
    uint8_t factor = 0;
    uint8_t divisor = 0;

    if (!serial || !serial->hport) return -USB_ERR_INVAL;
    setup = serial->hport->setup;

    if (priv) {
        memcpy(&priv->line_coding, line_coding, sizeof(struct cdc_line_coding));
    }

    switch (line_coding->bParityType) {
        case 0: break;
        case 1: reg_value |= CH341_L_PO; break;
        case 2: reg_value |= CH341_L_PE; break;
        case 3: reg_value |= CH341_L_PM; break;
        case 4: reg_value |= CH341_L_PS; break;
        default: return -USB_ERR_INVAL;
    }

    switch (line_coding->bDataBits) {
        case 5: reg_value |= CH341_L_D5; break;
        case 6: reg_value |= CH341_L_D6; break;
        case 7: reg_value |= CH341_L_D7; break;
        case 8: reg_value |= CH341_L_D8; break;
        default: return -USB_ERR_INVAL;
    }

    if (line_coding->bCharFormat == 2) {
        reg_value |= CH341_L_SB;
    }

    reg_value |= 0xC0;
    value |= 0x9c;
    value |= reg_value << 8;

    usbh_ch34x_get_baudrate_div(line_coding->dwDTERate, &factor, &divisor);

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH34X_SERIAL_INIT;
    setup->wValue = value;
    setup->wIndex = (factor << 8) | 0x80 | divisor;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ch34x_set_line_state(struct usbh_serial *serial, bool dtr, bool rts)
{
    struct usb_setup_packet *setup;
    struct ch34x_priv *priv = (struct ch34x_priv *)serial->priv;

    if (!serial || !serial->hport) return -USB_ERR_INVAL;
    setup = serial->hport->setup;

    priv->dtr_state = dtr;
    priv->rts_state = rts;

    uint16_t wValue = 0x0f;
    if (priv->dtr_state) wValue |= (1 << 5);
    if (priv->rts_state) wValue |= (1 << 6);

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH34X_MODEM_CTRL;
    setup->wValue = wValue;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ch34x_attach(struct usbh_serial *serial)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    struct ch34x_priv *priv;
    int ret;
    uint8_t *buffer = serial->io_buf;

    /* Allocate Private Data */
    priv = usb_osal_malloc(sizeof(struct ch34x_priv));
    if (!priv) return -USB_ERR_NOMEM;
    memset(priv, 0, sizeof(struct ch34x_priv));
    serial->priv = priv;

    /* Read Version */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH34X_READ_VERSION;
    setup->wValue = 0;
    setup->wIndex = 0;
    setup->wLength = 2;

    ret = usbh_control_transfer(serial->hport, setup, buffer);
    if (ret < 0) return ret;

    USB_LOG_INFO("Ch34x chip version %02x:%02x\r\n", buffer[0], buffer[1]);

    /* Flow Control Init */
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CH34X_WRITE_REG;
    setup->wValue = 0x2727;
    setup->wIndex = 0;
    setup->wLength = 0;

    ret = usbh_control_transfer(serial->hport, setup, NULL);
    return ret;
}

static void ch34x_detach(struct usbh_serial *serial)
{
    if (serial->priv) {
        usb_osal_free(serial->priv);
        serial->priv = NULL;
    }
}

static const struct usbh_serial_driver ch34x_drv = {
        .driver_name = "ch34x",
        .attach = ch34x_attach,
        .detach = ch34x_detach,
        .set_line_coding = ch34x_set_line_coding,
        .set_line_state = ch34x_set_line_state,
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