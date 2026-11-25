// SPDX-License-Identifier: MIT
#include <ntddk.h>
#include <wdf.h>
#include "session_report.h"
#include "pcie_caps.h"
#ifdef NEKO_MT7927_RING_STAGE
#include "ring_session_buffers.h"
#include "ring_link_caps.h"
#endif

// Unpublished buffer experiment. The lower filter receives the file lifetime
// from the upper function driver. It has no device-control handlers.
typedef struct {
    WDFWAITLOCK lock;
    WDFDMAENABLER enabler;
#ifdef NEKO_MT7927_RING_STAGE
    SESSION_RING_BUFFERS rings;
#else
    WDFCOMMONBUFFER buffer;
#endif
    WDFFILEOBJECT owner;
    SESSION_REPORT report;
} SESSION_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SESSION_DEVICE, GetSessionDevice)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SessionAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SessionPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE SessionRelease;
EVT_WDF_DEVICE_FILE_CREATE SessionCreate;
EVT_WDF_FILE_CLEANUP SessionCleanup;

static NTSTATUS WriteReport(WDFDEVICE device, SESSION_REPORT *report)
{
    WDF_DEVICE_PROPERTY_DATA property;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &sessionReportKey);
    return WdfDeviceAssignProperty(device, &property,
        report ? DEVPROP_TYPE_BINARY : DEVPROP_TYPE_EMPTY, report ? sizeof(*report) : 0, report);
}

// Caller holds lock. No descriptor or device has ever received this buffer.
static void ReleaseUnpublished(SESSION_DEVICE *context)
{
#ifdef NEKO_MT7927_RING_STAGE
    DeletePrivateRings(&context->rings);
#else
    WdfObjectDelete(context->buffer);
    context->buffer = NULL;
#endif
    context->owner = NULL;
    context->report.active = 0;
    context->report.heldBytes = 0;
}

NTSTATUS SessionPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(raw);
    UNREFERENCED_PARAMETER(translated);
    SESSION_DEVICE *context = GetSessionDevice(device);
    WdfWaitLockAcquire(context->lock, NULL);
    NTSTATUS status = STATUS_SUCCESS;
    if (!context->enabler) {
        WdfDeviceSetAlignmentRequirement(device, WdfDeviceGetAlignmentRequirement(device));
        WDF_DMA_ENABLER_CONFIG dma;
#ifdef NEKO_MT7927_RING_STAGE
        WDF_DMA_ENABLER_CONFIG_INIT(&dma, WdfDmaProfileScatterGatherDuplex,
                                     Mt7927SlabSlots * Mt7927SliceBytes);
#else
        WDF_DMA_ENABLER_CONFIG_INIT(&dma, WdfDmaProfileScatterGatherDuplex, SessionBufferBytes);
#endif
        dma.WdmDmaVersionOverride = 3;
        status = WdfDmaEnablerCreate(device, &dma, WDF_NO_OBJECT_ATTRIBUTES, &context->enabler);
    }
    if (NT_SUCCESS(status)) {
        LARGE_INTEGER now;
        KeQuerySystemTimePrecise(&now);
        context->report.version = SessionReportVersion;
        context->report.prepareTimeLow = now.LowPart;
        context->report.prepareTimeHigh = (ULONG)now.HighPart;
        context->report.ready = 1;
        status = WriteReport(device, &context->report);
        if (!NT_SUCCESS(status)) context->report.ready = 0;
    }
    WdfWaitLockRelease(context->lock);
    if (NT_SUCCESS(status)) {
        LabReportPcieCaps(device);
#ifdef NEKO_MT7927_RING_STAGE
        RingReportLinkCaps(device);
#endif
    }
    return status;
}

NTSTATUS SessionRelease(WDFDEVICE device, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(translated);
    SESSION_DEVICE *context = GetSessionDevice(device);
    WdfWaitLockAcquire(context->lock, NULL);
    context->report.ready = 0;
    if (context->owner) {
        ReleaseUnpublished(context);
        ++context->report.pnpReclaims;
    }
    NTSTATUS status = WriteReport(device, NULL);
    WdfWaitLockRelease(context->lock);
    LabClearPcieCaps(device);
#ifdef NEKO_MT7927_RING_STAGE
    RingClearLinkCaps(device);
#endif
    return status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_NOT_FOUND ?
           STATUS_SUCCESS : status;
}

static NTSTATUS OpenSession(WDFDEVICE device, WDFFILEOBJECT file)
{
    SESSION_DEVICE *context = GetSessionDevice(device);
    if (!file) return STATUS_INVALID_PARAMETER;
    if (!context->report.ready) return STATUS_DEVICE_NOT_READY;
    if (context->owner) return STATUS_SHARING_VIOLATION;

#ifdef NEKO_MT7927_RING_STAGE
    NTSTATUS status = AllocatePrivateRings(device, context->enabler, &context->rings,
                                           &context->report.heldBytes);
    if (!NT_SUCCESS(status)) return status;
#else
    WDF_COMMON_BUFFER_CONFIG config;
    WDF_COMMON_BUFFER_CONFIG_INIT(&config, max(WdfDeviceGetAlignmentRequirement(device), 15u));
    WDFCOMMONBUFFER buffer;
    NTSTATUS status = WdfCommonBufferCreateWithConfig(context->enabler, SessionBufferBytes,
                                                       &config, WDF_NO_OBJECT_ATTRIBUTES, &buffer);
    if (!NT_SUCCESS(status)) return status;
    if (WdfCommonBufferGetLength(buffer) != SessionBufferBytes) {
        WdfObjectDelete(buffer);
        return STATUS_DATA_ERROR;
    }

    context->buffer = buffer;
    context->report.heldBytes = SessionBufferBytes;
#endif
    context->owner = file;
    context->report.active = 1;
    ++context->report.opens;
    status = WriteReport(device, &context->report);
    if (!NT_SUCCESS(status)) {
        // Failed creates receive no EvtFileCleanup.
        ReleaseUnpublished(context);
        --context->report.opens;
    }
    return status;
}

VOID SessionCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file)
{
    SESSION_DEVICE *context = GetSessionDevice(device);
    WdfWaitLockAcquire(context->lock, NULL);
    NTSTATUS status = OpenSession(device, file);
    WdfWaitLockRelease(context->lock);
    WdfRequestComplete(request, status);
}

VOID SessionCleanup(WDFFILEOBJECT file)
{
    WDFDEVICE device = WdfFileObjectGetDevice(file);
    SESSION_DEVICE *context = GetSessionDevice(device);
    WdfWaitLockAcquire(context->lock, NULL);
    if (context->owner == file) {
        ReleaseUnpublished(context);
        ++context->report.cleanups;
        // Cleanup cannot return a status. Suppress further admissions if the
        // observer can no longer see the current lifecycle.
        if (!NT_SUCCESS(WriteReport(device, &context->report))) context->report.ready = 0;
    }
    WdfWaitLockRelease(context->lock);
}

NTSTATUS SessionAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
    WdfFdoInitSetFilter(init);
    WDF_FILEOBJECT_CONFIG files;
    WDF_FILEOBJECT_CONFIG_INIT(&files, SessionCreate, WDF_NO_EVENT_CALLBACK, SessionCleanup);
    files.FileObjectClass = WdfFileObjectWdfCannotUseFsContexts;
    files.AutoForwardCleanupClose = WdfFalse;
    WdfDeviceInitSetFileObjectConfig(init, &files, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SessionPrepare;
    pnp.EvtDeviceReleaseHardware = SessionRelease;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SESSION_DEVICE);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = device;
    return WdfWaitLockCreate(&lockAttributes, &GetSessionDevice(device)->lock);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, SessionAdd);
    return WdfDriverCreate(driver, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
