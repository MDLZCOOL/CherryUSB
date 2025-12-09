#include "usbh_serial_cdc_acm.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_serial_cdc_acm"
#include "usb_log.h"

struct cdc_acm_private {
    uint8_t data_intf;
#ifdef CONFIG_USBHOST_CDC_ACM_NOTIFY
    struct usb_endpoint_descriptor *intin;
    struct usbh_urb intin_urb;
    uint8_t int_buf[16];
#endif
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static struct cdc_acm_private g_cdc_acm_data[CONFIG_USBHOST_MAX_SERIAL_CLASS];

static struct cdc_acm_private *get_private(struct usbh_serial *serial)
{
    if (serial->minor < CONFIG_USBHOST_MAX_SERIAL_CLASS) {
        return &g_cdc_acm_data[serial->minor];
    }
    return NULL;
}

static int cdc_acm_set_line_coding(struct usbh_serial *serial, struct cdc_line_coding *line_coding)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    uint8_t *buf = serial->io_buf;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_SET_LINE_CODING;
    setup->wValue = 0;
    setup->wIndex = serial->intf;
    setup->wLength = 7;

    memcpy(buf, line_coding, sizeof(struct cdc_line_coding));

    return usbh_control_transfer(serial->hport, setup, buf);
}

static int cdc_acm_get_line_coding(struct usbh_serial *serial, struct cdc_line_coding *line_coding)
{
    struct usb_setup_packet *setup = serial->hport->setup;
    int ret;
    uint8_t *buf = serial->io_buf;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_GET_LINE_CODING;
    setup->wValue = 0;
    setup->wIndex = serial->intf;
    setup->wLength = 7;

    ret = usbh_control_transfer(serial->hport, setup, buf);
    if (ret < 0)
        return ret;

    memcpy(line_coding, buf, sizeof(struct cdc_line_coding));
    return 0;
}

static int cdc_acm_set_line_state(struct usbh_serial *serial, bool dtr, bool rts)
{
    struct usb_setup_packet *setup = serial->hport->setup;

    /*
     * SET_CONTROL_LINE_STATE (22h)
     * wValue: Control Signal Bitmap
     *   Bit 0: DTR
     *   Bit 1: RTS
     */
    uint16_t wValue = (dtr ? 1 : 0) | ((rts ? 1 : 0) << 1);

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_SET_CONTROL_LINE_STATE;
    setup->wValue = wValue;
    setup->wIndex = serial->intf;
    setup->wLength = 0;

    return usbh_control_transfer(serial->hport, setup, NULL);
}

static int cdc_acm_attach(struct usbh_serial *serial)
{
    struct cdc_acm_private *private = get_private(serial);
    struct usbh_hubport *hport = serial->hport;
    struct usb_endpoint_descriptor *ep_desc;
    uint8_t data_intf = serial->intf + 1;
    int i;

    if (!private)
        return -USB_ERR_NOMEM;
    memset(private, 0, sizeof(struct cdc_acm_private));

    if (data_intf >= hport->config.config_desc.bNumInterfaces) {
        USB_LOG_ERR("Missing Data Interface\r\n");
        return -USB_ERR_NODEV;
    }
    private->data_intf = data_intf;

    struct usbh_interface *iface = &hport->config.intf[data_intf];

    for (i = 0; i < iface->altsetting[0].intf_desc.bNumEndpoints; i++) {
        ep_desc = &iface->altsetting[0].ep[i].ep_desc;

        if (USB_GET_ENDPOINT_TYPE(ep_desc->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
            if (ep_desc->bEndpointAddress & 0x80) {
                if (!serial->bulkin) {
                    serial->bulkin = ep_desc;
                    USBH_EP_INIT(serial->bulkin, ep_desc);
                }
            } else {
                if (!serial->bulkout) {
                    serial->bulkout = ep_desc;
                    USBH_EP_INIT(serial->bulkout, ep_desc);
                }
            }
        }
    }

    if (!serial->bulkin || !serial->bulkout) {
        USB_LOG_ERR("Bulk endpoints not found on intf %d\r\n", data_intf);
        return -USB_ERR_NODEV;
    }

#ifdef CONFIG_USBHOST_CDC_ACM_NOTIFY
    struct usbh_interface *ctl_iface = &serial->hport->config.intf[serial->intf];
    struct usb_endpoint_descriptor *ep_desc;
    int ret;

    for (int i = 0; i < ctl_iface->altsetting[0].intf_desc.bNumEndpoints; i++) {
        ep_desc = &ctl_iface->altsetting[0].ep[i].ep_desc;

        if (USB_GET_ENDPOINT_TYPE(ep_desc->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT) {
            if (ep_desc->bEndpointAddress & 0x80) { // IN endpoint
                priv->intin = ep_desc;
                USBH_EP_INIT(priv->intin, ep_desc);
                break;
            }
        }
    }

    if (priv->intin) {
        USB_LOG_INFO("Found Interrupt IN endpoint\r\n");

        usbh_int_urb_fill(&priv->intin_urb,
                          serial->hport,
                          priv->intin,
                          priv->int_buf,
                          sizeof(priv->int_buf),
                          cdc_acm_int_callback,
                          serial);

        ret = usbh_submit_urb(&priv->intin_urb);
        if (ret < 0) {
            USB_LOG_WRN("Failed to submit int urb: %d\r\n", ret);
        }
    }
#endif

    return 0;
}

static void cdc_acm_detach(struct usbh_serial *serial)
{
    struct cdc_acm_private *private = get_private(serial);
    if (private) {
        memset(private, 0, sizeof(struct cdc_acm_private));
    }
}

static const struct usbh_serial_driver cdc_acm_drv = {
    .driver_name = "cdc_acm",
    .attach = cdc_acm_attach,
    .detach = cdc_acm_detach,
    .set_line_coding = cdc_acm_set_line_coding,
    .get_line_coding = cdc_acm_get_line_coding,
    .set_line_state = cdc_acm_set_line_state,
};

static int usbh_cdc_acm_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = usbh_serial_probe(hport, intf, &cdc_acm_drv);
    if (serial) {
        hport->config.intf[intf].priv = serial;
        struct cdc_acm_private *private = get_private(serial);
        if (private) {
            hport->config.intf[private->data_intf].priv = (void *)1; /* Dummy marker */
        }

        return 0;
    }
    return -USB_ERR_NOMEM;
}

static int usbh_cdc_acm_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_serial *serial = (struct usbh_serial *)hport->config.intf[intf].priv;

    if (serial) {
        struct cdc_acm_private *private = get_private(serial);
        if (private) {
            hport->config.intf[private->data_intf].priv = NULL;
        }

        usbh_serial_release(serial);
        hport->config.intf[intf].priv = NULL;
    }
    return 0;
}

static int usbh_cdc_data_connect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)hport;
    (void)intf;
    return 0;
}

static int usbh_cdc_data_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)hport;
    (void)intf;
    return 0;
}

const struct usbh_class_driver cdc_acm_class_driver = {
    .driver_name = "cdc_acm",
    .connect = usbh_cdc_acm_connect,
    .disconnect = usbh_cdc_acm_disconnect
};

const struct usbh_class_driver cdc_data_class_driver = {
    .driver_name = "cdc_data",
    .connect = usbh_cdc_data_connect,
    .disconnect = usbh_cdc_data_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info cdc_acm_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS,
    .bInterfaceClass = USB_DEVICE_CLASS_CDC,
    .bInterfaceSubClass = CDC_ABSTRACT_CONTROL_MODEL,
    .bInterfaceProtocol = 0x00,
    .id_table = NULL,
    .class_driver = &cdc_acm_class_driver
};

CLASS_INFO_DEFINE const struct usbh_class_info cdc_data_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS,
    .bInterfaceClass = USB_DEVICE_CLASS_CDC_DATA,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
    .id_table = NULL,
    .class_driver = &cdc_data_class_driver
};