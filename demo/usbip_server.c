/*
 * Copyright (c) 2026, CherryUSB
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usbh_core.h"

#include <errno.h>

#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define USBIP_SERVER_PORT 3240
#define USBIP_PATH_LEN 256
#define USBIP_BUSID_LEN 32
#define USBIP_MAX_XFER 16384
#define USBIP_DATA_XFER_TIMEOUT_MS 100

#define USBIP_VERSION 0x0111
#define USBIP_OP_REQ_DEVLIST 0x8005
#define USBIP_OP_REP_DEVLIST 0x0005
#define USBIP_OP_REQ_IMPORT 0x8003
#define USBIP_OP_REP_IMPORT 0x0003

#define USBIP_CMD_SUBMIT 0x00000001
#define USBIP_RET_SUBMIT 0x00000003
#define USBIP_CMD_UNLINK 0x00000002
#define USBIP_RET_UNLINK 0x00000004

struct usbip_op_common {
    uint16_t version;
    uint16_t code;
    uint32_t status;
} __PACKED;

struct usbip_op_devlist_reply {
    uint32_t ndev;
} __PACKED;

struct usbip_usb_interface {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
} __PACKED;

struct usbip_usb_device {
    char path[USBIP_PATH_LEN];
    char busid[USBIP_BUSID_LEN];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;
} __PACKED;

struct usbip_header_basic {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
} __PACKED;

struct usbip_cmd_submit {
    uint32_t transfer_flags;
    int32_t transfer_buffer_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t interval;
    uint8_t setup[8];
} __PACKED;

struct usbip_ret_submit {
    int32_t status;
    uint32_t actual_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t error_count;
    uint8_t setup[8];
} __PACKED;

struct usbip_cmd_unlink {
    uint32_t unlink_seqnum;
    uint8_t padding[24];
} __PACKED;

struct usbip_ret_unlink {
    int32_t status;
    uint8_t padding[24];
} __PACKED;

struct usbip_server {
    uint8_t busid;
    uint16_t port;
    int listen_fd;
    int client_fd;
    bool started;
    bool imported;
    uint8_t hub_index;
    uint8_t hub_port;
    struct usbh_hubport *hport;
    usb_osal_mutex_t lock;
    usb_osal_thread_t thread;
} g_usbip_server;

static int usbip_send_all(int fd, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t sent = 0;

    while (sent < len) {
        int ret = send(fd, p + sent, len - sent, 0);
        if (ret <= 0) {
            return -USB_ERR_IO;
        }
        sent += (uint32_t)ret;
    }
    return 0;
}

static int usbip_recv_all(int fd, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t recv_len = 0;

    while (recv_len < len) {
        int ret = recv(fd, p + recv_len, len - recv_len, 0);
        if (ret <= 0) {
            return -USB_ERR_IO;
        }
        recv_len += (uint32_t)ret;
    }
    return 0;
}

static void usbip_close_socket(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void usbip_fill_device_desc(struct usbip_usb_device *udev, struct usbh_hubport *hport)
{
    memset(udev, 0, sizeof(*udev));
    snprintf(udev->path, sizeof(udev->path), "/sys/devices/cherryusb/%u-%u", hport->bus->busid, hport->dev_addr);
    snprintf(udev->busid, sizeof(udev->busid), "%u-%u", hport->parent->index, hport->port);

    udev->busnum = htonl(hport->bus->busid + 1);
    udev->devnum = htonl(hport->dev_addr);
    udev->speed = htonl(hport->speed);
    udev->idVendor = htons(hport->device_desc.idVendor);
    udev->idProduct = htons(hport->device_desc.idProduct);
    udev->bcdDevice = htons(hport->device_desc.bcdDevice);
    udev->bDeviceClass = hport->device_desc.bDeviceClass;
    udev->bDeviceSubClass = hport->device_desc.bDeviceSubClass;
    udev->bDeviceProtocol = hport->device_desc.bDeviceProtocol;
    udev->bConfigurationValue = hport->config.config_desc.bConfigurationValue;
    udev->bNumConfigurations = hport->device_desc.bNumConfigurations;
    udev->bNumInterfaces = hport->config.config_desc.bNumInterfaces;
}

static int usbip_send_devlist(int fd, struct usbh_hubport *hport)
{
    struct usbip_op_common rep = {
            .version = htons(USBIP_VERSION),
            .code = htons(USBIP_OP_REP_DEVLIST),
            .status = htonl(0),
    };
    struct usbip_op_devlist_reply devlist = {
            .ndev = htonl(1),
    };
    struct usbip_usb_device udev;

    usbip_fill_device_desc(&udev, hport);

    if (usbip_send_all(fd, &rep, sizeof(rep)) < 0 ||
        usbip_send_all(fd, &devlist, sizeof(devlist)) < 0 ||
        usbip_send_all(fd, &udev, sizeof(udev)) < 0) {
        return -USB_ERR_IO;
    }

    for (uint8_t i = 0; i < hport->config.config_desc.bNumInterfaces; i++) {
        struct usbip_usb_interface uintf = {
                .bInterfaceClass = hport->config.intf[i].altsetting[0].intf_desc.bInterfaceClass,
                .bInterfaceSubClass = hport->config.intf[i].altsetting[0].intf_desc.bInterfaceSubClass,
                .bInterfaceProtocol = hport->config.intf[i].altsetting[0].intf_desc.bInterfaceProtocol,
                .padding = 0,
        };
        if (usbip_send_all(fd, &uintf, sizeof(uintf)) < 0) {
            return -USB_ERR_IO;
        }
    }
    return 0;
}

static int usbip_send_import(int fd, struct usbh_hubport *hport)
{
    struct usbip_op_common rep = {
            .version = htons(USBIP_VERSION),
            .code = htons(USBIP_OP_REP_IMPORT),
            .status = htonl(0),
    };
    struct usbip_usb_device udev;

    usbip_fill_device_desc(&udev, hport);

    if (usbip_send_all(fd, &rep, sizeof(rep)) < 0 ||
        usbip_send_all(fd, &udev, sizeof(udev)) < 0) {
        return -USB_ERR_IO;
    }

    return 0;
}

static struct usb_endpoint_descriptor *usbip_find_ep(struct usbh_hubport *hport, uint8_t ep_addr)
{
    for (uint8_t i = 0; i < hport->config.config_desc.bNumInterfaces; i++) {
        struct usbh_interface_altsetting *alt = &hport->config.intf[i].altsetting[0];
        for (uint8_t j = 0; j < alt->intf_desc.bNumEndpoints; j++) {
            if (alt->ep[j].ep_desc.bEndpointAddress == ep_addr) {
                return &alt->ep[j].ep_desc;
            }
        }
    }
    return NULL;
}

static int usbip_do_control(struct usbh_hubport *hport,
                            const struct usbip_cmd_submit *cmd,
                            bool dir_in,
                            uint8_t *transfer_buf,
                            uint32_t transfer_len,
                            uint32_t *actual_length)
{
    int ret;
    struct usb_setup_packet setup;
    memcpy(&setup, cmd->setup, 8);
    ret = usbh_control_transfer(hport, &setup, transfer_buf);
    if (ret < 0) {
        *actual_length = 0;
        return ret;
    }
    if (dir_in) {
        *actual_length = ret;
        if (*actual_length > transfer_len) {
            *actual_length = transfer_len;
        }
    } else {
        *actual_length = 0;
    }
    return 0;
}

static int usbip_do_data(struct usbh_hubport *hport,
                         struct usb_endpoint_descriptor *ep,
                         bool dir_in,
                         uint8_t *transfer_buf,
                         uint32_t transfer_len,
                         uint32_t interval,
                         uint32_t *actual_length)
{
    struct usbh_urb urb;
    int ret;
    uint32_t timeout = USBIP_DATA_XFER_TIMEOUT_MS;

    /* Keep polling latency low for HID interrupt IN endpoints */
    if (dir_in && USB_GET_ENDPOINT_TYPE(ep->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT) {
        uint32_t poll_ms = interval;
        if (poll_ms < 4) {
            poll_ms = 4;
        } else if (poll_ms > 32) {
            poll_ms = 32;
        }
        timeout = poll_ms * 2;
    } else if (!dir_in) {
        /* Avoid blocking forever so host-side UNLINK can be handled quickly */
        timeout = 1000;
    }

    memset(&urb, 0, sizeof(urb));
    if (USB_GET_ENDPOINT_TYPE(ep->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT) {
        usbh_int_urb_fill(&urb, hport, ep, transfer_buf, transfer_len, timeout, NULL, NULL);
    } else {
        usbh_bulk_urb_fill(&urb, hport, ep, transfer_buf, transfer_len, timeout, NULL, NULL);
    }
    urb.interval = interval;

    ret = usbh_submit_urb(&urb);
    if (ret < 0) {
        if (dir_in && ret == -USB_ERR_TIMEOUT) {
            *actual_length = 0;
            return 0;
        }
        *actual_length = 0;
        return ret;
    }

    *actual_length = urb.actual_length;
    if (!dir_in && *actual_length == 0 && transfer_len > 0) {
        *actual_length = transfer_len;
    }
    return 0;
}

static int usbip_handle_submit(int fd, struct usbip_server *server, const struct usbip_header_basic *req_hdr)
{
    struct usbip_cmd_submit cmd;
    struct usbip_header_basic rep_hdr;
    struct usbip_ret_submit rep;
    uint8_t *transfer_buf = NULL;
    struct usb_endpoint_descriptor *ep = NULL;
    struct usbh_hubport *hport;
    uint32_t transfer_len;
    uint32_t epnum;
    uint32_t direction;
    uint32_t actual_length = 0;
    int ret = 0;
    int submit_status = 0;
    if (usbip_recv_all(fd, &cmd, sizeof(cmd)) < 0) {
        return -USB_ERR_IO;
    }
    epnum = ntohl(req_hdr->ep);
    direction = ntohl(req_hdr->direction);
    if ((int32_t)ntohl(cmd.transfer_buffer_length) < 0) {
        return -USB_ERR_INVAL;
    }
    transfer_len = (uint32_t)ntohl(cmd.transfer_buffer_length);
    if (transfer_len > USBIP_MAX_XFER) {
        return -USB_ERR_INVAL;
    }
    if (transfer_len > 0) {
        transfer_buf = usb_osal_malloc(transfer_len);
        if (!transfer_buf) {
            return -USB_ERR_NOMEM;
        }
    }
    usb_osal_mutex_take(server->lock);
    hport = server->hport;
    usb_osal_mutex_give(server->lock);
    if (!hport || !hport->connected) {
        submit_status = -USB_ERR_NODEV;
        goto cleanup;
    }
    if (!direction && transfer_len > 0) {
        if (usbip_recv_all(fd, transfer_buf, transfer_len) < 0) {
            ret = -USB_ERR_IO;
            goto cleanup;
        }
    } else if (direction && transfer_len > 0) {
        memset(transfer_buf, 0, transfer_len);
    }
    if (epnum == 0) {
        submit_status = usbip_do_control(hport, &cmd, direction != 0, transfer_buf, transfer_len, &actual_length);
    } else {
        uint8_t ep_addr = (uint8_t)epnum | (direction ? 0x80 : 0x00);
        ep = usbip_find_ep(hport, ep_addr);
        if (!ep) {
            submit_status = -USB_ERR_INVAL;
        } else {
            submit_status = usbip_do_data(hport, ep, direction != 0, transfer_buf, transfer_len, ntohl(cmd.interval), &actual_length);
        }
    }
    memset(&rep_hdr, 0, sizeof(rep_hdr));
    rep_hdr.command = htonl(USBIP_RET_SUBMIT);
    rep_hdr.seqnum = req_hdr->seqnum;
    rep_hdr.devid = req_hdr->devid;
    rep_hdr.direction = req_hdr->direction;
    rep_hdr.ep = req_hdr->ep;
    memset(&rep, 0, sizeof(rep));
    rep.status = htonl((uint32_t)submit_status);
    rep.actual_length = htonl(actual_length);
    rep.start_frame = 0;
    rep.number_of_packets = 0;
    rep.error_count = 0;
    if (usbip_send_all(fd, &rep_hdr, sizeof(rep_hdr)) < 0 ||
        usbip_send_all(fd, &rep, sizeof(rep)) < 0) {
        ret = -USB_ERR_IO;
        goto cleanup;
    }
    if ((submit_status == 0) && direction && actual_length > 0) {
        if (usbip_send_all(fd, transfer_buf, actual_length) < 0) {
            ret = -USB_ERR_IO;
            goto cleanup;
        }
    }
    cleanup:

    if (transfer_buf) {
        usb_osal_free(transfer_buf);
    }
    return ret;
}

static int usbip_handle_unlink(int fd, const struct usbip_header_basic *req_hdr)
{
    struct usbip_cmd_unlink cmd;
    struct usbip_header_basic rep_hdr;
    struct usbip_ret_unlink rep;

    if (usbip_recv_all(fd, &cmd, sizeof(cmd)) < 0) {
        return -USB_ERR_IO;
    }

    memset(&rep_hdr, 0, sizeof(rep_hdr));
    rep_hdr.command = htonl(USBIP_RET_UNLINK);
    rep_hdr.seqnum = req_hdr->seqnum;
    rep_hdr.devid = req_hdr->devid;
    rep_hdr.direction = req_hdr->direction;
    rep_hdr.ep = req_hdr->ep;

    memset(&rep, 0, sizeof(rep));
    /*
     * This server currently processes URBs synchronously, so there is no
     * outstanding async submit to cancel here. Tell client "not found" to
     * avoid duplicate giveback warnings in vhci_hcd.
     */
    rep.status = htonl((uint32_t)(-ENOENT));

    if (usbip_send_all(fd, &rep_hdr, sizeof(rep_hdr)) < 0 ||
        usbip_send_all(fd, &rep, sizeof(rep)) < 0) {
        return -USB_ERR_IO;
    }
    return 0;
}

static int usbip_handle_mgmt(int fd, struct usbip_server *server)
{
    struct usbip_op_common req;
    struct usbh_hubport *hport;

    if (usbip_recv_all(fd, &req, sizeof(req)) < 0) {
        return -USB_ERR_IO;
    }

    usb_osal_mutex_take(server->lock);
    hport = server->hport;
    usb_osal_mutex_give(server->lock);
    if (!hport || !hport->connected) {
        return -USB_ERR_NODEV;
    }

    switch (ntohs(req.code)) {
        case USBIP_OP_REQ_DEVLIST:
            return usbip_send_devlist(fd, hport);
        case USBIP_OP_REQ_IMPORT: {
            char busid[USBIP_BUSID_LEN];
            if (usbip_recv_all(fd, busid, sizeof(busid)) < 0) {
                return -USB_ERR_IO;
            }
            server->imported = true;
            return usbip_send_import(fd, hport);
        }
        default:
            return -USB_ERR_NOTSUPP;
    }
}

static void usbip_server_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)
{
    struct usbip_server *server = (struct usbip_server *)CONFIG_USB_OSAL_THREAD_GET_ARGV;
    struct sockaddr_in addr;

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        USB_LOG_ERR("usbip socket create failed\r\n");
        goto out;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server->port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        USB_LOG_ERR("usbip bind failed:%d\r\n", errno);
        goto out;
    }

    if (listen(server->listen_fd, 1) < 0) {
        USB_LOG_ERR("usbip listen failed:%d\r\n", errno);
        goto out;
    }

    USB_LOG_INFO("usbip server listening on %u\r\n", server->port);

    while (1) {
        struct usbip_header_basic hdr;

        server->client_fd = accept(server->listen_fd, NULL, NULL);
        if (server->client_fd < 0) {
            usb_osal_msleep(100);
            continue;
        }

        USB_LOG_INFO("usbip client connected\r\n");
        while (1) {
            int ret;
            uint32_t prefix;

            ret = recv(server->client_fd, &prefix, sizeof(prefix), MSG_PEEK);
            if (ret <= 0) {
                break;
            }

            if (ret < (int)sizeof(prefix)) {
                continue;
            }

            if (((ntohl(prefix) >> 16) & 0xffff) == USBIP_VERSION) {
                ret = usbip_handle_mgmt(server->client_fd, server);
                if (ret < 0) {
                    break;
                }
                continue;
            }

            if (usbip_recv_all(server->client_fd, &hdr, sizeof(hdr)) < 0) {
                break;
            }

            switch (ntohl(hdr.command)) {
                case USBIP_CMD_SUBMIT:
                    ret = usbip_handle_submit(server->client_fd, server, &hdr);
                    break;
                case USBIP_CMD_UNLINK:
                    ret = usbip_handle_unlink(server->client_fd, &hdr);
                    break;
                default:
                    ret = -USB_ERR_NOTSUPP;
                    break;
            }
            if (ret < 0) {
                break;
            }
        }

        USB_LOG_WRN("usbip client disconnected\r\n");
        server->imported = false;
        usbip_close_socket(&server->client_fd);
    }
    out:
    usbip_close_socket(&server->client_fd);
    usbip_close_socket(&server->listen_fd);
    usb_osal_thread_delete(NULL);
}

void usbip_server_event(uint8_t busid, uint8_t hub_index, uint8_t hub_port, uint8_t intf, uint8_t event)
{
    if (busid != g_usbip_server.busid) {
        return;
    }

    if (event == USBH_EVENT_DEVICE_CONFIGURED) {
        usb_osal_mutex_take(g_usbip_server.lock);
        g_usbip_server.hub_index = hub_index;
        g_usbip_server.hub_port = hub_port;
        g_usbip_server.hport = usbh_find_hubport(busid, hub_index, hub_port);
        usb_osal_mutex_give(g_usbip_server.lock);

        if (g_usbip_server.hport) {
            USB_LOG_INFO("usbip export ready: bus %u hub %u port %u devaddr %u\r\n",
                         busid, hub_index, hub_port, g_usbip_server.hport->dev_addr);
        }
    } else if (event == USBH_EVENT_DEVICE_DISCONNECTED) {
        usb_osal_mutex_take(g_usbip_server.lock);
        g_usbip_server.hport = NULL;
        g_usbip_server.imported = false;
        usb_osal_mutex_give(g_usbip_server.lock);
    }
}

int usbip_server_start(uint8_t busid, uint16_t port)
{
    memset(&g_usbip_server, 0, sizeof(g_usbip_server));

    g_usbip_server.busid = busid;
    g_usbip_server.port = port ? port : USBIP_SERVER_PORT;
    g_usbip_server.listen_fd = -1;
    g_usbip_server.client_fd = -1;

    g_usbip_server.lock = usb_osal_mutex_create();
    if (!g_usbip_server.lock) {
        return -USB_ERR_NOMEM;
    }

    g_usbip_server.thread = usb_osal_thread_create("usbip_srv",
                                                   8192,
                                                   CONFIG_USBHOST_PSC_PRIO,
                                                   usbip_server_thread,
                                                   &g_usbip_server);
    if (!g_usbip_server.thread) {
        usb_osal_mutex_delete(g_usbip_server.lock);
        g_usbip_server.lock = NULL;
        return -USB_ERR_NOMEM;
    }

    g_usbip_server.started = true;
    return 0;
}
