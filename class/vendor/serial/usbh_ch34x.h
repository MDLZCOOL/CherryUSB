/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBH_CH34X_H
#define USBH_CH34X_H

#include "usbh_serial.h"

/* flags for IO-Bits */
#define CH341_BIT_RTS (1 << 6)
#define CH341_BIT_DTR (1 << 5)

/******************************/
/* interrupt pipe definitions */
/******************************/
/* always 4 interrupt bytes */
/* first irq byte normally 0x08 */
/* second irq byte base 0x7d + below */
/* third irq byte base 0x94 + below */
/* fourth irq byte normally 0xee */

/* second interrupt byte */
#define CH341_MULT_STAT 0x04 /* multiple status since last interrupt event */

/* status returned in third interrupt answer byte, inverted in data
   from irq */
#define CH341_BIT_CTS         0x01
#define CH341_BIT_DSR         0x02
#define CH341_BIT_RI          0x04
#define CH341_BIT_DCD         0x08
#define CH341_BITS_MODEM_STAT 0x0f /* all bits */

#define CH341_REQ_READ_VERSION 0x5F
#define CH341_REQ_WRITE_REG    0x9A
#define CH341_REQ_READ_REG     0x95
#define CH341_REQ_SERIAL_INIT  0xA1
#define CH341_REQ_MODEM_CTRL   0xA4

#define CH341_REG_BREAK     0x05
#define CH341_REG_PRESCALER 0x12
#define CH341_REG_DIVISOR   0x13
#define CH341_REG_LCR       0x18
#define CH341_REG_LCR2      0x25
#define CH341_REG_FLOW_CTL  0x27

#define CH341_NBREAK_BITS 0x01

#define CH341_LCR_ENABLE_RX   0x80
#define CH341_LCR_ENABLE_TX   0x40
#define CH341_LCR_MARK_SPACE  0x20
#define CH341_LCR_PAR_EVEN    0x10
#define CH341_LCR_ENABLE_PAR  0x08
#define CH341_LCR_STOP_BITS_2 0x04
#define CH341_LCR_CS8         0x03
#define CH341_LCR_CS7         0x02
#define CH341_LCR_CS6         0x01
#define CH341_LCR_CS5         0x00

#define CH341_FLOW_CTL_NONE   0x00
#define CH341_FLOW_CTL_RTSCTS 0x01

#endif /* USBH_CH34X_H */