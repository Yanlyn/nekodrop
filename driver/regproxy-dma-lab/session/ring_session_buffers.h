/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "patch_ring_image.h"

// Stored in the device context rather than on the kernel stack.
typedef struct {
    WDFCOMMONBUFFER handles[Mt7927ImageParts];
    MT7927_IMAGE_BACKING backing;
    MT7927_IMAGE_PLAN plan;
    unsigned allocated;
} SESSION_RING_BUFFERS;

static void DeletePrivateRings(SESSION_RING_BUFFERS *rings)
{
    while (rings->allocated) {
        --rings->allocated;
        WdfObjectDelete(rings->handles[rings->allocated]);
    }
    RtlZeroMemory(rings, sizeof(*rings));
}

static NTSTATUS AllocatePrivateRings(WDFDEVICE device, WDFDMAENABLER enabler,
                                      SESSION_RING_BUFFERS *rings, ULONG *heldBytes)
{
    WDF_COMMON_BUFFER_CONFIG config;
    WDF_COMMON_BUFFER_CONFIG_INIT(&config, max(WdfDeviceGetAlignmentRequirement(device), 15u));
    ULONG total = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (unsigned i = 0; i < Mt7927ImageParts; ++i) {
        size_t bytes = mt7927_image_part_bytes(i);
        WDFCOMMONBUFFER handle;
        status = WdfCommonBufferCreateWithConfig(enabler, bytes, &config,
                                                  WDF_NO_OBJECT_ATTRIBUTES, &handle);
        if (!NT_SUCCESS(status)) break;
        rings->handles[rings->allocated++] = handle;
        MT7927_IMAGE_VIEW *view = &rings->backing.parts[i];
        view->bytes = WdfCommonBufferGetLength(handle);
        if (view->bytes != bytes) {
            status = STATUS_DATA_ERROR;
            break;
        }
        view->cpu = (uint8_t *)WdfCommonBufferGetAlignedVirtualAddress(handle);
        view->logical = (uint64_t)WdfCommonBufferGetAlignedLogicalAddress(handle).QuadPart;
        total += (ULONG)bytes;
    }
    if (NT_SUCCESS(status)) {
        // The three candidates stay in private metadata; every TX slot remains idle.
        rings->plan = mt7927_build_patch_ring_image(&rings->backing, 1);
        if (!rings->plan.prepared) status = STATUS_DATA_ERROR;
    }
    if (!NT_SUCCESS(status)) {
        DeletePrivateRings(rings);
        return status;
    }
    *heldBytes = total;
    return STATUS_SUCCESS;
}
