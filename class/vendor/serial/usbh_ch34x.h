/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBH_CH34X_H
#define USBH_CH34X_H

#include "usbh_serial.h"

/* Requests */
#define CH34X_READ_VERSION 0x5F
#define CH34X_WRITE_REG    0x9A
#define CH34X_READ_REG     0x95
#define CH34X_SERIAL_INIT  0xA1
#define CH34X_MODEM_CTRL   0xA4

// modem control bits
#define CH34X_BIT_RTS (1 << 6)
#define CH34X_BIT_DTR (1 << 5)

#define CH341_CTO_O   0x10
#define CH341_CTO_D   0x20
#define CH341_CTO_R   0x40
#define CH341_CTI_C   0x01
#define CH341_CTI_DS  0x02
#define CH341_CTRL_RI 0x04
#define CH341_CTI_DC  0x08
#define CH341_CTI_ST  0x0f

#define CH341_L_ER 0x80
#define CH341_L_ET 0x40
#define CH341_L_PS 0x38
#define CH341_L_PM 0x28
#define CH341_L_PE 0x18
#define CH341_L_PO 0x08
#define CH341_L_SB 0x04
#define CH341_L_D8 0x03
#define CH341_L_D7 0x02
#define CH341_L_D6 0x01
#define CH341_L_D5 0x00

/* CH34X Private Data */
struct ch34x_priv {
    struct cdc_line_coding line_coding;
    uint8_t dtr_state;
    uint8_t rts_state;
};

#endif /* USBH_CH34X_H */