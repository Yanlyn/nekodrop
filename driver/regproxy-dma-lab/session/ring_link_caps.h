// SPDX-License-Identifier: MIT
#pragma once
#include "pcie_caps.h"

// A prepare-time observation of the endpoint only. This describes link
// configuration before command submission. It cannot establish DMA retirement.
typedef struct {
    uint32_t version, timeLow, timeHigh, scan;
    uint32_t aspmEnabled, transactionPending, linkSpeed, linkWidth, aspmSupported;
} RING_LINK_REPORT;

static unsigned RingReadLinkCaps(LAB_PCIE_READ read, void *context, RING_LINK_REPORT *out)
{
    LAB_PCIE_REPORT standard = {0};
    unsigned at = 0;
    unsigned scan = LabReadPcieStandardAt(read, context, &standard, &at);
    if (scan != PcieScanComplete) return scan;
    uint32_t deviceStatus, linkCaps, linkCtl, linkStatus;
    if (!read(context, at + 0x0a, 2, &deviceStatus) ||
        !read(context, at + 0x0c, 4, &linkCaps) ||
        !read(context, at + 0x10, 2, &linkCtl) ||
        !read(context, at + 0x12, 2, &linkStatus))
        return PcieReadFailed;
    if (deviceStatus == UINT16_MAX || linkCaps == UINT32_MAX || linkCtl == UINT16_MAX ||
        linkStatus == UINT16_MAX || !(linkStatus & 15u) || !(linkStatus & 0x3f0u))
        return PcieReadFailed;
    out->aspmEnabled = linkCtl & 3u;
    out->transactionPending = (deviceStatus >> 5) & 1u;
    out->linkSpeed = linkStatus & 15u;
    out->linkWidth = (linkStatus >> 4) & 63u;
    out->aspmSupported = (linkCaps >> 10) & 3u;
    return PcieScanComplete;
}

#ifndef LAB_PCIE_CAPS_PARSER_TEST
static const DEVPROPKEY ringLinkKey = {
    {0xb81473c0, 0x37ab, 0x48a1, {0xb1, 0x9d, 0xe5, 0xf6, 0x74, 0x70, 0x13, 0xff}}, 5
};
C_ASSERT(sizeof(RING_LINK_REPORT) == 36);

static void RingReportLinkCaps(WDFDEVICE device)
{
    LARGE_INTEGER now;
    KeQuerySystemTimePrecise(&now);
    RING_LINK_REPORT report = {1, now.LowPart, (ULONG)now.HighPart, PcieBusUnavailable, 0, 0, 0, 0, 0};
    BUS_INTERFACE_STANDARD bus = {0};
    if (NT_SUCCESS(WdfFdoQueryForInterface(device, &GUID_BUS_INTERFACE_STANDARD,
                                          (PINTERFACE)&bus, sizeof(bus), 1, NULL))) {
        if (bus.GetBusData) report.scan = RingReadLinkCaps(LabPcieBusRead, &bus, &report);
        if (bus.InterfaceDereference) bus.InterfaceDereference(bus.Context);
    }
    WDF_DEVICE_PROPERTY_DATA property;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &ringLinkKey);
    (void)WdfDeviceAssignProperty(device, &property, DEVPROP_TYPE_BINARY, sizeof(report), &report);
}

static void RingClearLinkCaps(WDFDEVICE device)
{
    WDF_DEVICE_PROPERTY_DATA property;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &ringLinkKey);
    (void)WdfDeviceAssignProperty(device, &property, DEVPROP_TYPE_EMPTY, 0, NULL);
}
#endif
