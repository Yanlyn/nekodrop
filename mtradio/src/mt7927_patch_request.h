/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux mt76 8d3ae59288f1: mt76_connac_mcu.c (patch_sem_ctrl),
 * mt7925/mcu.c (fill_message), and mt76_connac3_mac.h.
 * Copyright (C) 2020, 2023 MediaTek Inc.
 * Original BSD-3-Clause-Clear notice: mtradio/LICENSE.mt7927-mt76. */
#pragma once
#include <stdint.h>

/* The PCIe path queues PATCH_SEM_CONTROL on WM. The buffer holds one
 * 64-byte legacy MCU TX header followed by a little-endian 32-bit operation. */
typedef struct {
  uint8_t bytes[68];
} MT7927_PATCH_REQUEST;

/* Return 1 for a legal request, 0 for a null destination or invalid sequence.
 * An invalid sequence leaves the destination unchanged. */
static inline int mt7927_make_patch_release_request(unsigned sequence, MT7927_PATCH_REQUEST *out) {
  MT7927_PATCH_REQUEST request = {{0}};
  if (!out || sequence < 1 || sequence > 15) return 0;

  request.bytes[0] = (uint8_t)sizeof(request.bytes); /* TXD0 TX_BYTES */
  request.bytes[3] = 0x41; /* TXD0 PKT_FMT=CMD, Q_IDX=MCU_PORT_RX_Q0 */
  request.bytes[5] = 0x40; /* Connac3 TXD1 HDR_FORMAT=CMD, bits 15:14 */
  request.bytes[32] = (uint8_t)(sizeof(request.bytes) - 32); /* MCU len excludes eight TXD words */
  request.bytes[35] = 0x80; /* MCU_PQ_ID(1, 0x20) = 0x8000 */
  request.bytes[36] = 0x10; /* PATCH_SEM_CONTROL */
  request.bytes[37] = 0xa0; /* MCU_PKT_ID */
  request.bytes[38] = 3; /* MCU_Q_NA */
  request.bytes[39] = (uint8_t)sequence;
  /* H2N, extended command/ACK fields, reserved bytes, and PATCH_SEM_RELEASE are zero. */
  *out = request;
  return 1;
}

static inline int mt7927_make_patch_get_request(unsigned sequence, MT7927_PATCH_REQUEST *out) {
  if (!mt7927_make_patch_release_request(sequence, out)) return 0;
  out->bytes[64] = 1; /* PATCH_SEM_GET is the only field that differs from RELEASE. */
  return 1;
}
