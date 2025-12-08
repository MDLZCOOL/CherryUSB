/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_cp210x.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_cp210x"
#include "usb_log.h"

static int cp210x_enable(struct usbh_serial *serial)
{
    struct usb_setup_packet *setup = serial->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CP210X_IFC_ENABLE;
    setup->wValue = 1;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int cp210x_set_flow(struct usbh_serial *serial)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    uint8_t *buf = serial->io_buf;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CP210X_SET_FLOW;
    setup->wValue = 0;
    setup->wIndex = serial->intf;
    setup->wLength = 16;

    /* CP210x Flow Control State Setting/Response (16 bytes)
     * 0-3:  ulControlHandshake
     * 4-7:  ulFlowReplace
     * 8-11: ulXonLimit
     * 12-15: ulXoffLimit
     */
    memset(buf, 0, 16);
    buf[13] = 0x20;

    return usbh_control_transfer(serial->hport, setup, buf);
}

static int cp210x_set_chars(struct usbh_serial *serial)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    uint8_t *buf = serial->io_buf;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CP210X_SET_CHARS;
    setup->wValue = 0;
    setup->wIndex = serial->intf;
    setup->wLength = 6;

    /*
     * CP210x Special Characters Response (6 bytes)
     * 0: bEofChar
     * 1: bErrorChar
     * 2: bBreakChar
     * 3: bEventChar
     * 4: bXonChar
     * 5: bXoffChar
     */
    memset(buf, 0, 6);
    buf[0] = 0x80;
    buf[4] = 0x88;
    buf[5] = 0x28;

    return usbh_control_transfer(serial->hport, setup, buf);
}

static int cp210x_set_baudrate(struct usbh_serial *serial, uint32_t baudrate)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    uint8_t *buf = serial->io_buf;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CP210X_SET_BAUDRATE;
    setup->wValue = 0;
    setup->wIndex = serial->intf;
    setup->wLength = 4;

    memcpy(buf, (uint8_t *)&baudrate, 4);

    return usbh_control_transfer(serial->hport, setup, buf);
}

static int cp210x_set_line_coding(struct usbh_serial *serial, struct cdc_line_coding *line_coding)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    int ret;
    uint16_t value;

    if (!serial || !serial->hport)
        return -USB_ERR_INVAL;

    ret = cp210x_set_baudrate(serial, line_coding->dwDTERate);
    if (ret < 0)
        return ret;

    /*
     * Bits 15-8: Word length, legal values are 5, 6, 7 and 8.
     * Bits 7-4:  Parity setting:
     *              0 = none.
     *              1 = odd.
     *              2 = even.
     *              3 = mark.
     *              4 = space.
     *              other values reserved.
     * Bits 3-0:  Stop bits:
     *              0 = 1 stop bit
     *              1 = 1.5 stop bits
     *              2 = 2 stop bits
     *              other values reserved.
     */
    value = ((line_coding->bDataBits & 0x0F) << 8) |
            ((line_coding->bParityType & 0x0F) << 4) |
            ((line_coding->bCharFormat & 0x0F) << 0);

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CP210X_SET_LINE_CTL;
    setup->wValue = value;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int cp210x_set_line_state(struct usbh_serial *serial, bool dtr, bool rts)
{
    struct usb_setup_packet *setup;
    struct cp210x_priv *priv = (struct cp210x_priv *)serial->priv;
    uint16_t value;

    if (!serial || !serial->hport || !priv) {
        return -USB_ERR_INVAL;
    }
    setup = serial->hport->setup;

    priv->dtr_state = dtr;
    priv->rts_state = rts;

    /*
     * CP210X_SET_MHS
     * wValue High Byte: Control Mask (1=Write this bit)
     *       Bit 1: RTS Mask
     *       Bit 0: DTR Mask
     * wValue Low Byte:  State
     *       Bit 1: RTS State
     *       Bit 0: DTR State
     *
     * We want to write BOTH DTR and RTS (Mask = 0x0300)
     */
    value = 0x0300;
    if (dtr)
        value |= 0x01;
    if (rts)
        value |= 0x02;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CP210X_SET_MHS;
    setup->wValue = value;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int cp210x_attach(struct usbh_serial *serial)
{
    struct cp210x_priv *priv;
    int ret;

    /* Allocate Private Data */
    priv = usb_osal_malloc(sizeof(struct cp210x_priv));
    if (!priv)
        return -USB_ERR_NOMEM;
    memset(priv, 0, sizeof(struct cp210x_priv));
    serial->priv = priv;

    /* Hardware Init Sequence */
    ret = cp210x_enable(serial);
    if (ret < 0)
        return ret;

    ret = cp210x_set_flow(serial);
    if (ret < 0)
        return ret;

    ret = cp210x_set_chars(serial);
    if (ret < 0)
        return ret;

    return 0;
}

static void cp210x_detach(struct usbh_serial *serial)
{
    if (serial->priv) {
        usb_osal_free(serial->priv);
        serial->priv = NULL;
    }
}

static const struct usbh_serial_driver cp210x_drv = {
    .driver_name = "cp210x",
    .attach = cp210x_attach,
    .detach = cp210x_detach,
    .set_line_coding = cp210x_set_line_coding,
    .set_line_state = cp210x_set_line_state,
};

static int usbh_cp210x_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = usbh_serial_probe(hport, intf, &cp210x_drv);
    if (serial) {
        hport->config.intf[intf].priv = serial;
        return 0;
    }
    return -USB_ERR_NOMEM;
}

static int usbh_cp210x_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = (struct usbh_serial *)hport->config.intf[intf].priv;
    if (serial) {
        usbh_serial_release(serial);
        hport->config.intf[intf].priv = NULL;
    }
    return 0;
}

static const uint16_t cp210x_id_table[][2] = {
    { 0x10C4, 0xEA60 },
    { 0, 0 },
};

const struct usbh_class_driver cp210x_class_driver = {
    .driver_name = "cp210x",
    .connect = usbh_cp210x_connect,
    .disconnect = usbh_cp210x_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info cp210x_class_info = {
    .match_flags = USB_CLASS_MATCH_VID_PID | USB_CLASS_MATCH_INTF_CLASS,
    .bInterfaceClass = 0xff,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
    .id_table = cp210x_id_table,
    .class_driver = &cp210x_class_driver
};