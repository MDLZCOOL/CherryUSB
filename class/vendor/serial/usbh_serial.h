#ifndef USBH_SERIAL_H
#define USBH_SERIAL_H

#include "usbh_core.h"
#include "usb_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*usbh_serial_rx_cb_t)(void *arg, uint8_t *data, uint32_t len);

struct usbh_serial;

/**
 * @brief Serial Driver Operations
 */
struct usbh_serial_driver {
    const char *driver_name;

    int (*attach)(struct usbh_serial *serial);
    void (*detach)(struct usbh_serial *serial);

    int (*set_line_coding)(struct usbh_serial *serial, struct cdc_line_coding *line_coding);
    int (*get_line_coding)(struct usbh_serial *serial, struct cdc_line_coding *line_coding);
    int (*set_line_state)(struct usbh_serial *serial, bool dtr, bool rts);
    int (*set_flow_control)(struct usbh_serial *serial, bool enable);
};

/**
 * @brief Serial Instance
 */
struct usbh_serial {
    struct usbh_hubport *hport;
    uint8_t intf;  /* Interface Number */
    uint8_t minor; /* Serial Port Number (/dev/ttyUSBx) */

    struct usb_endpoint_descriptor *bulkin;  /* Bulk IN endpoint */
    struct usb_endpoint_descriptor *bulkout; /* Bulk OUT endpoint */
    struct usbh_urb bulkout_urb;
    struct usbh_urb bulkin_urb;

    const struct usbh_serial_driver *driver;
    uint8_t *io_buf;
    usbh_serial_rx_cb_t rx_cb;
    void *rx_cb_arg;
};

struct usbh_serial *usbh_serial_probe(struct usbh_hubport *hport, uint8_t intf, const struct usbh_serial_driver *driver);
void usbh_serial_release(struct usbh_serial *serial);
int usbh_serial_set_line_coding(struct usbh_serial *serial, uint32_t baudrate, uint8_t databits, uint8_t parity, uint8_t stopbits);
int usbh_serial_set_flow_control(struct usbh_serial *serial, bool enable);
int usbh_serial_set_dtr_rts(struct usbh_serial *serial, bool dtr, bool rts);
int usbh_serial_write(struct usbh_serial *serial, const uint8_t *buffer, uint32_t buflen, uint32_t timeout);
int usbh_serial_read(struct usbh_serial *serial, uint8_t *buffer, uint32_t buflen, uint32_t timeout);
int usbh_serial_read_async(struct usbh_serial *serial, uint8_t *buffer, uint32_t buflen, usbh_serial_rx_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* USBH_SERIAL_H */