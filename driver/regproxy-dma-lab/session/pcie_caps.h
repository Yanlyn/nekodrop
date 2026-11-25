// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stddef.h>

// Passive, point-in-time configuration summary of the session device. Flags
// describe the evidence obtained so far; only PcieScanComplete permits a
// classification. Even a complete report says nothing about pending writes.
enum LAB_PCIE_SCAN {
    PcieScanComplete = 1, PcieBusUnavailable, PcieReadFailed, PcieWrongDevice,
    PcieBadStandardChain, PcieUnsupportedEndpoint, PcieBadExtendedChain,
    PcieAerNotObserved
};
enum LAB_PCIE_FACT {
    PcieFnZero = 1u, PcieSingleFunction = 2u, PcieControlsRead = 4u,
    PcieRelaxedAllowed = 8u, PcieIdoRequestAllowed = 16u, PcieIdoCompletionAllowed = 32u,
    PcieAerSeen = 64u, PcieVcSeen = 128u, PcieMfvcSeen = 256u, PcieVc9Seen = 512u,
    PcieTc0ByCapability = 1024u
};
typedef struct {
    uint32_t version, timeLow, timeHigh, scan, facts, expressVersion, extendedHeaders;
} LAB_PCIE_REPORT;

// Read only the requested bytes into a zero-extended little-endian value.
typedef int (*LAB_PCIE_READ)(void *context, unsigned offset, unsigned bytes, uint32_t *value);

static unsigned LabReadPcieStandardAt(LAB_PCIE_READ read, void *context, LAB_PCIE_REPORT *r,
                                      unsigned *expressOffset)
{
    uint32_t ids, subsystem, header, status, ptr;
    uint64_t seen = 0;
    unsigned pcie = 0;
    if (!read(context, 0, 4, &ids) || !read(context, 0x0e, 1, &header))
        return PcieReadFailed;
    if (ids != 0x792714c3 || (header & 0x7f) != 0) return PcieWrongDevice;
    if (!read(context, 0x2c, 4, &subsystem)) return PcieReadFailed;
    if (subsystem != 0xe0fe105b) return PcieWrongDevice;
    if (!(header & 0x80)) r->facts |= PcieSingleFunction;
    if (!read(context, 6, 2, &status) || !read(context, 0x34, 1, &ptr)) return PcieReadFailed;
    if (status == UINT16_MAX || !(status & 0x10)) return PcieBadStandardChain;
    while (ptr) {
        if (ptr < 0x40 || ptr > 0xfc || (ptr & 3)) return PcieBadStandardChain;
        uint64_t bit = UINT64_C(1) << (ptr / 4);
        if (seen & bit) return PcieBadStandardChain;
        seen |= bit;
        uint32_t cap;
        if (!read(context, ptr, 2, &cap)) return PcieReadFailed;
        if ((cap & 0xff) == 0xff) return PcieBadStandardChain;
        if ((cap & 0xff) == 0x10) {
            if (pcie) return PcieBadStandardChain;
            pcie = ptr;
        }
        ptr = (cap >> 8) & 0xff;
    }
    uint32_t caps, ctl, ctl2;
    if (!pcie || pcie + 0x2a > 0x100) return PcieUnsupportedEndpoint;
    // Reject a malformed list that would place a control read in another capability.
    for (unsigned delta = 4; delta <= 0x28; delta += 4)
        if (seen & (UINT64_C(1) << ((pcie + delta) / 4))) return PcieBadStandardChain;
    if (!read(context, pcie + 2, 2, &caps)) return PcieReadFailed;
    r->expressVersion = caps & 0xf;
    if (r->expressVersion != 2 || ((caps >> 4) & 0xf) != 0) return PcieUnsupportedEndpoint;
    if (!read(context, pcie + 8, 2, &ctl) || !read(context, pcie + 0x28, 2, &ctl2))
        return PcieReadFailed;
    if (ctl == UINT16_MAX || ctl2 == UINT16_MAX) return PcieReadFailed;
    r->facts |= PcieControlsRead;
    if (ctl & 0x0010) r->facts |= PcieRelaxedAllowed;
    if (ctl2 & 0x0100) r->facts |= PcieIdoRequestAllowed;
    if (ctl2 & 0x0200) r->facts |= PcieIdoCompletionAllowed;
    if (expressOffset) *expressOffset = pcie;
    return PcieScanComplete;
}

static unsigned LabReadPcieStandard(LAB_PCIE_READ read, void *context, LAB_PCIE_REPORT *r)
{
    return LabReadPcieStandardAt(read, context, r, NULL);
}

static unsigned LabReadPcieCaps(LAB_PCIE_READ read, void *context, LAB_PCIE_REPORT *r)
{
    unsigned status = LabReadPcieStandard(read, context, r);
    if (status != PcieScanComplete) return status;
    // Budget at most 64 extended headers on this board. Following an aligned
    // backwards link is allowed unless a previously visited header repeats.
    uint32_t seen[32] = {0};
    unsigned ptr = 0x100;
    while (ptr) {
        if (ptr < 0x100 || ptr > 0xffc || (ptr & 3) || r->extendedHeaders == 64)
            return PcieBadExtendedChain;
        unsigned index = ptr / 4;
        uint32_t mask = UINT32_C(1) << (index % 32);
        if (seen[index / 32] & mask) return PcieBadExtendedChain;
        seen[index / 32] |= mask;
        uint32_t cap;
        if (!read(context, ptr, 4, &cap)) return PcieReadFailed;
        unsigned id = cap & 0xffff;
        if (!id || id == 0xffff || !(cap & 0xf0000)) return PcieBadExtendedChain;
        ++r->extendedHeaders;
        switch (id) {
        case 0x01: r->facts |= PcieAerSeen; break;
        case 0x02: r->facts |= PcieVcSeen; break;
        case 0x08: r->facts |= PcieMfvcSeen; break;
        case 0x09: r->facts |= PcieVc9Seen; break;
        }
        ptr = cap >> 20;
    }
    // The test board exposes AER in its regular PCI devnode properties; require that
    // positive control rather than infer an absence from inaccessible space.
    if (!(r->facts & PcieAerSeen)) return PcieAerNotObserved;
    const unsigned relevant = PcieSingleFunction | PcieFnZero | PcieVcSeen | PcieMfvcSeen | PcieVc9Seen;
    if ((r->facts & relevant) == (PcieSingleFunction | PcieFnZero)) r->facts |= PcieTc0ByCapability;
    return PcieScanComplete;
}

#ifndef LAB_PCIE_CAPS_PARSER_TEST
#include <initguid.h>
#include <wdmguid.h>

static const DEVPROPKEY labPcieCapsKey = {
    {0xb81473c0, 0x37ab, 0x48a1, {0xb1, 0x9d, 0xe5, 0xf6, 0x74, 0x70, 0x13, 0xff}}, 4
};
C_ASSERT(sizeof(LAB_PCIE_REPORT) == 28);

static int LabPcieBusRead(void *context, unsigned offset, unsigned bytes, uint32_t *value)
{
    BUS_INTERFACE_STANDARD *bus = context;
    *value = 0;
    return bus->GetBusData(bus->Context, PCI_WHICHSPACE_CONFIG, value, offset, bytes) == bytes;
}

static void LabReportPcieCaps(WDFDEVICE device)
{
    LARGE_INTEGER now;
    KeQuerySystemTimePrecise(&now);
    LAB_PCIE_REPORT report = {1, now.LowPart, (ULONG)now.HighPart, PcieBusUnavailable, 0, 0, 0};
    ULONG address = 0, length = 0;
    if (NT_SUCCESS(WdfDeviceQueryProperty(device, DevicePropertyAddress,
                                          sizeof(address), &address, &length)) &&
        length == sizeof(address) && (address & 0xffff) == 0)
        report.facts |= PcieFnZero;
    BUS_INTERFACE_STANDARD bus = {0};
    if (NT_SUCCESS(WdfFdoQueryForInterface(device, &GUID_BUS_INTERFACE_STANDARD,
                                          (PINTERFACE)&bus, sizeof(bus), 1, NULL))) {
        if (bus.GetBusData) report.scan = LabReadPcieCaps(LabPcieBusRead, &bus, &report);
        if (bus.InterfaceDereference) bus.InterfaceDereference(bus.Context);
    }
    WDF_DEVICE_PROPERTY_DATA property;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &labPcieCapsKey);
    (void)WdfDeviceAssignProperty(device, &property, DEVPROP_TYPE_BINARY, sizeof(report), &report);
}

static void LabClearPcieCaps(WDFDEVICE device)
{
    WDF_DEVICE_PROPERTY_DATA property;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &labPcieCapsKey);
    (void)WdfDeviceAssignProperty(device, &property, DEVPROP_TYPE_EMPTY, 0, NULL);
}
#endif
