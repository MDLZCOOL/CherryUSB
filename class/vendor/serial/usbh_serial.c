#include "usbh_serial.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_serial"
#include "usb_log.h"

#define DEV_FORMAT         "/dev/ttyUSB%d"
#define DEV_FORMAT_CDC_ACM "/dev/ttyACM%d"
#define GET_SERIAL_DEV_FMT(driver_name) \
    ((driver_name && strcmp(driver_name, "cdc_acm") == 0) ? DEV_FORMAT_CDC_ACM : DEV_FORMAT)

/* Pool allocator for serial instances */
#define SERIAL_IOBUF_SIZE               64
#define CONFIG_USBHOST_MAX_SERIAL_CLASS 4
static struct usbh_serial g_serial_class[CONFIG_USBHOST_MAX_SERIAL_CLASS];
static uint32_t g_devinuse = 0;
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_serial_io_bufs[CONFIG_USBHOST_MAX_SERIAL_CLASS][USB_ALIGN_UP(SERIAL_IOBUF_SIZE, CONFIG_USB_ALIGN_SIZE)];

static struct usbh_serial *usbh_serial_alloc(void)
{
    uint8_t devno;
    for (devno = 0; devno < CONFIG_USBHOST_MAX_SERIAL_CLASS; devno++) {
        if ((g_devinuse & (1U << devno)) == 0) {
            g_devinuse |= (1U << devno);
            memset(&g_serial_class[devno], 0, sizeof(struct usbh_serial));
            g_serial_class[devno].minor = devno;
            g_serial_class[devno].io_buf = g_serial_io_bufs[devno];
            return &g_serial_class[devno];
        }
    }
    return NULL;
}

static void usbh_serial_free(struct usbh_serial *serial)
{
    uint8_t devno = serial->minor;
    if (devno < 32) {
        g_devinuse &= ~(1U << devno);
    }
}

static void usbh_serial_urb_rx_cb(void *arg, int nbytes)
{
    struct usbh_serial *serial = (struct usbh_serial *)arg;
    struct usbh_urb *urb = &serial->bulkin_urb;
    int len = nbytes;

    if (!serial || !serial->rx_cb) {
        return;
    }

    usbh_serial_rx_cb_t user_cb = serial->rx_cb;
    void *user_arg = serial->rx_cb_arg;

    serial->rx_cb = NULL;
    serial->rx_cb_arg = NULL;

    if (len > 0) {
        user_cb(user_arg, urb->transfer_buffer, (uint32_t)len);
    }
}

int usbh_serial_write(struct usbh_serial *serial, const uint8_t *buffer, uint32_t buflen, uint32_t timeout)
{
    int ret;
    struct usbh_urb *urb;

    if (!serial || !serial->bulkout) {
        return -USB_ERR_INVAL;
    }
    urb = &serial->bulkout_urb;

    usbh_bulk_urb_fill(urb, serial->hport, serial->bulkout, (uint8_t *)buffer, buflen, timeout, NULL, NULL);
    ret = usbh_submit_urb(urb);

    if (ret == 0) {
        ret = urb->actual_length;
    }
    return ret;
}

int usbh_serial_read(struct usbh_serial *serial, uint8_t *buffer, uint32_t buflen, uint32_t timeout)
{
    int ret;
    struct usbh_urb *urb;

    if (!serial || !serial->bulkin) {
        return -USB_ERR_INVAL;
    }
    if (serial->rx_cb) {
        return -USB_ERR_BUSY;
    }

    urb = &serial->bulkin_urb;

    usbh_bulk_urb_fill(urb, serial->hport, serial->bulkin, buffer, buflen, timeout, NULL, NULL);
    ret = usbh_submit_urb(urb);

    if (ret == 0) {
        ret = urb->actual_length;
    } else if (ret == -USB_ERR_TIMEOUT) {
        if (urb->actual_length > 0) {
            ret = urb->actual_length;
        } else {
            ret = -USB_ERR_TIMEOUT;
        }
    }

    return ret;
}

int usbh_serial_read_async(struct usbh_serial *serial, uint8_t *buffer, uint32_t buflen,
                           usbh_serial_rx_cb_t cb, void *arg)
{
    struct usbh_urb *urb;
    int ret;

    if (!serial || !serial->bulkin || !buffer || !cb)
        return -USB_ERR_INVAL;
    if (serial->rx_cb)
        return -USB_ERR_BUSY;

    serial->rx_cb = cb;
    serial->rx_cb_arg = arg;

    urb = &serial->bulkin_urb;

    usbh_bulk_urb_fill(urb, serial->hport, serial->bulkin, buffer, buflen,
                       0, usbh_serial_urb_rx_cb, serial);

    ret = usbh_submit_urb(urb);
    if (ret < 0) {
        serial->rx_cb = NULL;
        return ret;
    }
    return 0;
}

struct usbh_serial *usbh_serial_probe(struct usbh_hubport *hport, uint8_t intf,
                                      const struct usbh_serial_driver *driver)
{
    struct usb_endpoint_descriptor *ep_desc;
    struct usbh_serial *serial;
    int ret;

    serial = usbh_serial_alloc();
    if (serial == NULL) {
        USB_LOG_ERR("Fail to alloc serial class\r\n");
        return NULL;
    }

    serial->hport = hport;
    serial->intf = intf;
    serial->driver = driver;

    for (uint8_t i = 0; i < hport->config.intf[intf].altsetting[0].intf_desc.bNumEndpoints; i++) {
        ep_desc = &hport->config.intf[intf].altsetting[0].ep[i].ep_desc;

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

    if (driver->attach) {
        ret = driver->attach(serial);
        if (ret < 0) {
            USB_LOG_ERR("Serial attach failed: %d\r\n", ret);
            usbh_serial_free(serial);
            return NULL;
        }
    }

    if (!serial->bulkin || !serial->bulkout) {
        USB_LOG_ERR("Serial endpoints not found\r\n");
        usbh_serial_free(serial);
        return NULL;
    }

    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN, GET_SERIAL_DEV_FMT(driver->driver_name), serial->minor);
    USB_LOG_INFO("Register USB Serial: %s (%s)\r\n", hport->config.intf[intf].devname, driver->driver_name);

    return serial;
}

void usbh_serial_release(struct usbh_serial *serial)
{
    if (!serial)
        return;

    serial->rx_cb = NULL;
    serial->rx_cb_arg = NULL;

    if (serial->bulkin) {
        usbh_kill_urb(&serial->bulkin_urb);
    }
    if (serial->bulkout) {
        usbh_kill_urb(&serial->bulkout_urb);
    }

    if (serial->driver && serial->driver->detach) {
        serial->driver->detach(serial);
    }

    USB_LOG_INFO("Unregister USB Serial: %s (%s)\r\n", serial->hport->config.intf[serial->intf].devname, serial->driver->driver_name);
    usbh_serial_free(serial);
}

int usbh_serial_set_line_coding(struct usbh_serial *serial, uint32_t baudrate,
                                uint8_t databits, uint8_t parity, uint8_t stopbits)
{
    struct cdc_line_coding coding;
    coding.dwDTERate = baudrate;
    coding.bDataBits = databits;
    coding.bParityType = parity;
    coding.bCharFormat = stopbits;

    if (serial && serial->driver && serial->driver->set_line_coding) {
        return serial->driver->set_line_coding(serial, &coding);
    }
    return -USB_ERR_NOTSUPP;
}

int usbh_serial_set_flow_control(struct usbh_serial *serial, bool enable)
{
    if (serial && serial->driver && serial->driver->set_flow_control) {
        return serial->driver->set_flow_control(serial, enable);
    }
    if (enable) {
        return -USB_ERR_NOTSUPP;
    }
    return 0;
}

int usbh_serial_set_dtr_rts(struct usbh_serial *serial, bool dtr, bool rts)
{
    if (serial && serial->driver && serial->driver->set_line_state) {
        return serial->driver->set_line_state(serial, dtr, rts);
    }
    return -USB_ERR_NOTSUPP;
}