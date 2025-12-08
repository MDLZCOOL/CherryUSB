/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_ftdi.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_ftdi"

#include "usb_log.h"

static const char *ftdi_chip_name[] = {
    [SIO] = "SIO",
    [FT232A] = "FT232A",
    [FT232B] = "FT232B",
    [FT2232C] = "FT2232C/D",
    [FT232R] = "FT232R",
    [FT232H] = "FT232H",
    [FT2232H] = "FT2232H",
    [FT4232H] = "FT4232H",
    [FT4232HA] = "FT4232HA",
    [FT232HP] = "FT232HP",
    [FT233HP] = "FT233HP",
    [FT2232HP] = "FT2232HP",
    [FT2233HP] = "FT2233HP",
    [FT4232HP] = "FT4232HP",
    [FT4233HP] = "FT4233HP",
    [FTX] = "FT-X",
};

/*
 * Divide positive or negative dividend by positive or negative divisor
 * and round to the closest integer. Result is undefined for negative
 * divisors if the dividend variable type is unsigned and for negative
 * dividends if the divisor variable type is unsigned.
 */
#define DIV_ROUND_CLOSEST(x, divisor) (       \
    {                                         \
        typeof(x) __x = x;                    \
        typeof(divisor) __d = divisor;        \
        (((typeof(x))-1) > 0 ||               \
         ((typeof(divisor))-1) > 0 ||         \
         (((__x) > 0) == ((__d) > 0))) ?      \
            (((__x) + ((__d) / 2)) / (__d)) : \
            (((__x) - ((__d) / 2)) / (__d));  \
    })

static uint32_t ftdi_232bm_baud_base_to_divisor(uint32_t baud, int base)
{
    static const unsigned char divfrac[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
    uint32_t divisor;
    int divisor3 = DIV_ROUND_CLOSEST(base, 2 * baud);
    divisor = divisor3 >> 3;
    divisor |= (uint32_t)divfrac[divisor3 & 0x7] << 14;
    if (divisor == 1)
        divisor = 0;
    else if (divisor == 0x4001)
        divisor = 1;
    return divisor;
}

static uint32_t ftdi_232bm_baud_to_divisor(uint32_t baud)
{
    return ftdi_232bm_baud_base_to_divisor(baud, 48000000);
}

static uint32_t ftdi_2232h_baud_base_to_divisor(uint32_t baud, int base)
{
    static const unsigned char divfrac[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
    uint32_t divisor;
    int divisor3;

    divisor3 = DIV_ROUND_CLOSEST(8 * base, 10 * baud);
    divisor = divisor3 >> 3;
    divisor |= (uint32_t)divfrac[divisor3 & 0x7] << 14;
    if (divisor == 1)
        divisor = 0;
    else if (divisor == 0x4001)
        divisor = 1;
    divisor |= 0x00020000;
    return divisor;
}

static uint32_t ftdi_2232h_baud_to_divisor(uint32_t baud)
{
    return ftdi_2232h_baud_base_to_divisor(baud, 120000000);
}

static int ftdi_reset(struct usbh_serial *serial)
{
    struct usb_setup_packet *setup = serial->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = SIO_RESET_REQUEST;
    setup->wValue = 0;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ftdi_set_modem(struct usbh_serial *serial, uint16_t value)
{
    struct usb_setup_packet *setup = serial->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = SIO_SET_MODEM_CTRL_REQUEST;
    setup->wValue = value;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ftdi_set_baudrate(struct usbh_serial *serial, uint32_t baudrate)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    struct ftdi_priv *priv = (struct ftdi_priv *)serial->priv;
    uint32_t div_value;
    uint16_t value;
    uint8_t baudrate_high;

    if (!priv) {
        return -USB_ERR_INVAL;
    }

    switch (priv->chip_type) {
        case FT232B:
        case FT2232C:
        case FT232R:
            if (baudrate > 3000000) {
                return -USB_ERR_INVAL;
            }
            div_value = ftdi_232bm_baud_to_divisor(baudrate);
            break;
        default:
            if ((baudrate <= 12000000) && (baudrate >= 1200)) {
                div_value = ftdi_2232h_baud_to_divisor(baudrate);
            } else {
                return -USB_ERR_INVAL;
            }
            break;
    }

    value = div_value & 0xFFFF;
    baudrate_high = (div_value >> 16) & 0xff;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = SIO_SET_BAUDRATE_REQUEST;
    setup->wValue = value;
    setup->wIndex = (baudrate_high << 8) | serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ftdi_set_data_format(struct usbh_serial *serial, uint8_t databits, uint8_t parity, uint8_t stopbits, uint8_t isbreak)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    uint16_t value;

    value = ((isbreak & 0x01) << 14) | ((stopbits & 0x03) << 11) | ((parity & 0x0f) << 8) | (databits & 0x0f);

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = SIO_SET_DATA_REQUEST;
    setup->wValue = value;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ftdi_set_latency_timer(struct usbh_serial *serial, uint16_t value)
{
    struct usb_setup_packet *setup = serial->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = SIO_SET_LATENCY_TIMER_REQUEST;
    setup->wValue = value;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ftdi_set_flow_ctrl(struct usbh_serial *serial, uint16_t value)
{
    struct usb_setup_packet *setup = serial->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = SIO_SET_FLOW_CTRL_REQUEST;
    setup->wValue = value;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int ftdi_read_modem_status(struct usbh_serial *serial)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    struct ftdi_priv *priv = (struct ftdi_priv *)serial->priv;
    int ret;
    uint8_t *buf = serial->io_buf;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = SIO_POLL_MODEM_STATUS_REQUEST;
    setup->wValue = 0x0000;
    setup->wIndex = serial->intf;
    setup->wLength = 2;

    ret = usbh_control_transfer(serial->hport, setup, buf);
    if (ret < 0)
        return ret;

    if (priv) {
        memcpy(priv->modem_status, buf, 2);
    }
    return ret;
}

static int ftdi_set_line_coding(struct usbh_serial *serial, struct cdc_line_coding *line_coding)
{
    int ret;

    ret = ftdi_set_baudrate(serial, line_coding->dwDTERate);
    if (ret < 0)
        return ret;

    return ftdi_set_data_format(serial, line_coding->bDataBits, line_coding->bParityType, line_coding->bCharFormat, 0);
}

static int ftdi_set_line_state(struct usbh_serial *serial, bool dtr, bool rts)
{
    int ret;

    if (dtr) {
        ret = ftdi_set_modem(serial, SIO_SET_DTR_HIGH);
    } else {
        ret = ftdi_set_modem(serial, SIO_SET_DTR_LOW);
    }
    if (ret < 0)
        return ret;

    if (rts) {
        ret = ftdi_set_modem(serial, SIO_SET_RTS_HIGH);
    } else {
        ret = ftdi_set_modem(serial, SIO_SET_RTS_LOW);
    }

    return ret;
}

static int ftdi_attach(struct usbh_serial *serial)
{
    struct ftdi_priv *priv;
    uint16_t bcdDevice;
    int ret;

    /* Allocate Private Data */
    priv = usb_osal_malloc(sizeof(struct ftdi_priv));
    if (!priv)
        return -USB_ERR_NOMEM;
    memset(priv, 0, sizeof(struct ftdi_priv));
    serial->priv = priv;

    /* Determine Chip Type based on bcdDevice */
    bcdDevice = serial->hport->device_desc.bcdDevice;
    switch (bcdDevice) {
        case 0x400:
            priv->chip_type = FT232B;
            break;
        case 0x500:
            priv->chip_type = FT2232C;
            break;
        case 0x600:
            priv->chip_type = FT232R;
            break;
        case 0x700:
            priv->chip_type = FT2232H;
            break;
        case 0x800:
            priv->chip_type = FT4232H;
            break;
        case 0x900:
            priv->chip_type = FT232H;
            break;
        default:
            USB_LOG_WRN("Unknown FTDI chip version:%04x, defaulting to FT232R\r\n", bcdDevice);
            priv->chip_type = FT232R;
            break;
    }
    USB_LOG_INFO("FTDI Chip: %s\r\n", ftdi_chip_name[priv->chip_type]);

    /* Hardware Init Sequence */
    ret = ftdi_reset(serial);
    if (ret < 0)
        return ret;

    ret = ftdi_set_flow_ctrl(serial, SIO_DISABLE_FLOW_CTRL);
    if (ret < 0)
        return ret;

    ret = ftdi_set_latency_timer(serial, 0x10);
    if (ret < 0)
        return ret;

    ftdi_read_modem_status(serial);

    return 0;
}

static void ftdi_detach(struct usbh_serial *serial)
{
    if (serial->priv) {
        usb_osal_free(serial->priv);
        serial->priv = NULL;
    }
}

static int ftdi_bulk_in_process(struct usbh_serial *serial, uint8_t *buf, uint32_t len)
{
    if (len < 2)
        return 0;

    memmove(buf, buf + 2, len - 2);
    return len - 2;
}

static const struct usbh_serial_driver ftdi_drv = {
    .driver_name = "ftdi",
    .attach = ftdi_attach,
    .detach = ftdi_detach,
    .set_line_coding = ftdi_set_line_coding,
    .set_line_state = ftdi_set_line_state,
    .bulk_in_process = ftdi_bulk_in_process,
};

static int usbh_ftdi_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = usbh_serial_probe(hport, intf, &ftdi_drv);
    if (serial) {
        hport->config.intf[intf].priv = serial;
        return 0;
    }
    return -USB_ERR_NOMEM;
}

static int usbh_ftdi_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = (struct usbh_serial *)hport->config.intf[intf].priv;
    if (serial) {
        usbh_serial_release(serial);
        hport->config.intf[intf].priv = NULL;
    }
    return 0;
}

static const uint16_t ftdi_id_table[][2] = {
    { 0x0403, 0x6001 },
    { 0x0403, 0x6010 },
    { 0, 0 },
};

const struct usbh_class_driver ftdi_class_driver = {
    .driver_name = "ftdi",
    .connect = usbh_ftdi_connect,
    .disconnect = usbh_ftdi_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info ftdi_class_info = {
    .match_flags = USB_CLASS_MATCH_VID_PID | USB_CLASS_MATCH_INTF_CLASS,
    .bInterfaceClass = 0xff,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
    .id_table = ftdi_id_table,
    .class_driver = &ftdi_class_driver
};