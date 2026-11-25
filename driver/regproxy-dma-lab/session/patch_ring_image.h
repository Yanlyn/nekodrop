/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux mt76 8d3ae59288f1: mt7925/{pci.c,mt7925.h,pci_mcu.c}, mt792x_core.c,
 * mt76_connac_mcu.c and dma.{c,h}. Copyright (C) 2016 Felix Fietkau;
 * Copyright (C) 2023 MediaTek Inc. See mtradio/LICENSE.mt7927-mt76.
 * Offline x64 Windows host image. No driver entry point or device access. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "../../../mtradio/src/mt7927_patch_request.h"

enum Mt7927ImagePart {
    Mt7927Tx0, Mt7927Tx15, Mt7927Tx16, Mt7927Rx4, Mt7927Rx6, Mt7927Rx7,
    Mt7927Commands, Mt7927Rx4Slabs, Mt7927Rx6Slabs = Mt7927Rx4Slabs + 96,
    Mt7927Rx7Slabs = Mt7927Rx6Slabs + 32, Mt7927ImageParts = Mt7927Rx7Slabs + 32
};
enum {
    Mt7927SlabSlots = 16, Mt7927SliceBytes = 2048, Mt7927CommandStride = 80,
    Mt7927DescriptorBytes = 16
};

typedef struct {
    uint8_t *cpu;
    uint64_t logical;
    size_t bytes;
} MT7927_IMAGE_VIEW;

/* Supply fresh, unpublished common buffers from a single device DMA enabler.
 * The owner must retain all handles; device provenance cannot be inferred
 * from a numeric address. Store the view table in owner memory, not on a
 * small kernel stack. Slabs mirror the original 32 KiB common-buffer test. */
typedef struct {
    MT7927_IMAGE_VIEW parts[Mt7927ImageParts];
} MT7927_IMAGE_BACKING;

typedef struct {
    uint32_t buf0, ctrl, buf1, info;
} MT7927_IMAGE_DESCRIPTOR;

typedef struct {
    unsigned prepared;
    /* Private candidates for TX15 slots 0,1,2, respectively. The TX ring
     * itself remains idle. POWER requests no firmware response. The owner
     * must observe the power state before GET and a matching grant before
     * RELEASE. Merely constructing this object performs none of those steps. */
    MT7927_IMAGE_DESCRIPTOR power, get, release;
} MT7927_IMAGE_PLAN;

static inline size_t mt7927_image_part_bytes(unsigned part)
{
    static const unsigned ring_slots[] = {2048, 256, 128, 1536, 512, 512};
    if (part < Mt7927Commands) return ring_slots[part] * Mt7927DescriptorBytes;
    if (part == Mt7927Commands) return 3 * Mt7927CommandStride;
    if (part >= Mt7927ImageParts) return 0;
    /* Linux posts ndesc-1 RX slots. Each ring's last slab is one slot short. */
    unsigned slots = (part + 1 == Mt7927Rx6Slabs || part + 1 == Mt7927Rx7Slabs ||
                      part + 1 == Mt7927ImageParts) ? Mt7927SlabSlots - 1 : Mt7927SlabSlots;
    return slots * Mt7927SliceBytes;
}

static inline int mt7927_image_overlap(uint64_t a, size_t an, uint64_t b, size_t bn)
{
    return a <= b ? b - a < an : a - b < bn;
}

static inline int mt7927_image_valid(const MT7927_IMAGE_BACKING *backing)
{
    if (!backing) return 0;
    for (unsigned i = 0; i < Mt7927ImageParts; ++i) {
        const MT7927_IMAGE_VIEW *v = &backing->parts[i];
        size_t n = mt7927_image_part_bytes(i);
        uintptr_t cpu = (uintptr_t)v->cpu;
        if (!cpu || v->bytes != n || ((cpu | v->logical) & 15) ||
            cpu > UINTPTR_MAX - (n - 1) || v->logical > UINT32_MAX - (n - 1) ||
            mt7927_image_overlap(cpu, n, (uintptr_t)backing, sizeof(*backing))) return 0;
        for (unsigned j = 0; j < i; ++j) {
            const MT7927_IMAGE_VIEW *other = &backing->parts[j];
            if (mt7927_image_overlap(v->logical, n, other->logical, other->bytes) ||
                mt7927_image_overlap(cpu, n, (uintptr_t)other->cpu, other->bytes)) return 0;
        }
    }
    return 1;
}

static inline uint8_t mt7927_image_next_seq(unsigned seq)
{
    return (uint8_t)(seq == 15 ? 1 : seq + 1);
}

static inline MT7927_IMAGE_PLAN
mt7927_build_patch_ring_image(const MT7927_IMAGE_BACKING *backing, unsigned power_seq)
{
    MT7927_IMAGE_PLAN plan = {0};
    if (power_seq < 1 || power_seq > 15 || !mt7927_image_valid(backing)) return plan;
    /* The x64 Windows target is little endian; use memcpy to access mapped
     * storage as bytes independently of C effective-type or packing rules. */
    for (unsigned i = 0; i < Mt7927ImageParts; ++i) {
        const MT7927_IMAGE_VIEW *v = &backing->parts[i];
        memset(v->cpu, 0, v->bytes);
        if (i < Mt7927Commands) {
            const uint32_t done = UINT32_C(1) << 31;
            for (size_t at = 0; at < v->bytes; at += Mt7927DescriptorBytes)
                memcpy(v->cpu + at + 4, &done, sizeof(done));
        }
    }

    static const unsigned first_slabs[] = {Mt7927Rx4Slabs, Mt7927Rx6Slabs, Mt7927Rx7Slabs, Mt7927ImageParts};
    for (unsigned r = 0; r < 3; ++r) {
        uint8_t *descriptor = backing->parts[Mt7927Rx4 + r].cpu;
        for (unsigned s = first_slabs[r]; s < first_slabs[r + 1]; ++s) {
            const MT7927_IMAGE_VIEW *slab = &backing->parts[s];
            for (size_t at = 0; at < slab->bytes; at += Mt7927SliceBytes) {
                /* Windows owns all 2048 bytes. Linux separately subtracts
                 * skb_shared_info from its 2048-byte host allocation. Validating
                 * the full slab above bounds each contiguous slice and its end. */
                MT7927_IMAGE_DESCRIPTOR rx = {(uint32_t)(slab->logical + at), Mt7927SliceBytes << 16, 0, 0};
                memcpy(descriptor, &rx, sizeof(rx));
                descriptor += Mt7927DescriptorBytes;
            }
        }
    }

    const MT7927_IMAGE_VIEW *commands = &backing->parts[Mt7927Commands];
    MT7927_PATCH_REQUEST request;
    const uint8_t get_seq = mt7927_image_next_seq(power_seq);
    mt7927_make_patch_get_request(power_seq, &request);
    /* Both fixed wire images use the same legacy MCU envelope and a payload
     * of 01 00 00 00. NIC_POWER_CTRL is CID 0x04, mt76_connac_mcu_restart. */
    request.bytes[36] = 0x04;
    memcpy(commands->cpu, request.bytes, sizeof(request.bytes));
    mt7927_make_patch_get_request(get_seq, &request);
    memcpy(commands->cpu + Mt7927CommandStride, request.bytes, sizeof(request.bytes));
    mt7927_make_patch_release_request(mt7927_image_next_seq(get_seq), &request);
    memcpy(commands->cpu + 2 * Mt7927CommandStride, request.bytes, sizeof(request.bytes));

    MT7927_IMAGE_DESCRIPTOR tx = {(uint32_t)commands->logical,
        (uint32_t)(sizeof(request.bytes) << 16) | (UINT32_C(1) << 30), 0, 0};
    plan.power = tx;
    tx.buf0 += Mt7927CommandStride;
    plan.get = tx;
    tx.buf0 += Mt7927CommandStride;
    plan.release = tx;
    plan.prepared = 1;
    return plan;
}

#if defined(__cplusplus)
static_assert(sizeof(MT7927_IMAGE_DESCRIPTOR) == Mt7927DescriptorBytes, "MT7927 WFDMA descriptor size");
#else
_Static_assert(sizeof(MT7927_IMAGE_DESCRIPTOR) == Mt7927DescriptorBytes, "MT7927 WFDMA descriptor size");
#endif
