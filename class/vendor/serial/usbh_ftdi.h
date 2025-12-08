/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBH_FTDI_H
#define USBH_FTDI_H

#include "usbh_serial.h"

/* Requests */
#define SIO_RESET_REQUEST             0x00 /* Reset the port */
#define SIO_SET_MODEM_CTRL_REQUEST    0x01 /* Set the modem control register */
#define SIO_SET_FLOW_CTRL_REQUEST     0x02 /* Set flow control register */
#define SIO_SET_BAUDRATE_REQUEST      0x03 /* Set baud rate */
#define SIO_SET_DATA_REQUEST          0x04 /* Set the data characteristics of the port */
#define SIO_POLL_MODEM_STATUS_REQUEST 0x05
#define SIO_SET_EVENT_CHAR_REQUEST    0x06
#define SIO_SET_ERROR_CHAR_REQUEST    0x07
#define SIO_SET_LATENCY_TIMER_REQUEST 0x09
#define SIO_GET_LATENCY_TIMER_REQUEST 0x0A
#define SIO_SET_BITMODE_REQUEST       0x0B
#define SIO_READ_PINS_REQUEST         0x0C
#define SIO_READ_EEPROM_REQUEST       0x90
#define SIO_WRITE_EEPROM_REQUEST      0x91
#define SIO_ERASE_EEPROM_REQUEST      0x92

#define SIO_DISABLE_FLOW_CTRL 0x0
#define SIO_RTS_CTS_HS        (0x1 << 8)
#define SIO_DTR_DSR_HS        (0x2 << 8)
#define SIO_XON_XOFF_HS       (0x4 << 8)

#define SIO_SET_DTR_MASK 0x1
#define SIO_SET_DTR_HIGH (1 | (SIO_SET_DTR_MASK << 8))
#define SIO_SET_DTR_LOW  (0 | (SIO_SET_DTR_MASK << 8))
#define SIO_SET_RTS_MASK 0x2
#define SIO_SET_RTS_HIGH (2 | (SIO_SET_RTS_MASK << 8))
#define SIO_SET_RTS_LOW  (0 | (SIO_SET_RTS_MASK << 8))

enum ftdi_chip_type {
    SIO,
    FT232A,
    FT232B,
    FT2232C,
    FT232R,
    FT232H,
    FT2232H,
    FT4232H,
    FT4232HA,
    FT232HP,
    FT233HP,
    FT2232HP,
    FT2233HP,
    FT4232HP,
    FT4233HP,
    FTX,
};

/* FTDI Private Data */
struct ftdi_priv {
    enum ftdi_chip_type chip_type;
    uint8_t modem_status[2];
};

#endif /* USBH_FTDI_H */