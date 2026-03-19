/*
 * Copyright (c) 2026, CherryUSB
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBIP_SERVER_H
#define USBIP_SERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int usbip_server_start(uint8_t busid, uint16_t port);
void usbip_server_event(uint8_t busid, uint8_t hub_index, uint8_t hub_port, uint8_t intf, uint8_t event);

#ifdef __cplusplus
}
#endif

#endif
