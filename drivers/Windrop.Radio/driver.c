// SPDX-License-Identifier: GPL-2.0-only
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#include "radio.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD WindropDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE WindropPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE WindropReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY WindropD0Entry;
EVT_WDF_DEVICE_D0_EXIT WindropD0Exit;
EVT_WDF_FILE_CLEANUP WindropFileCleanup;
EVT_WDF_DEVICE_QUERY_REMOVE WindropQueryRemove;
EVT_WDF_DEVICE_SURPRISE_REMOVAL WindropSurpriseRemoval;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL WindropIoControl;

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, WindropDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
        &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS WindropDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_FILEOBJECT_CONFIG file;
    WDF_DMA_ENABLER_CONFIG dma;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware = WindropPrepareHardware;
    power.EvtDeviceReleaseHardware = WindropReleaseHardware;
    power.EvtDeviceD0Entry = WindropD0Entry;
    power.EvtDeviceD0Exit = WindropD0Exit;
    power.EvtDeviceQueryRemove = WindropQueryRemove;
    power.EvtDeviceSurpriseRemoval = WindropSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &power);
    WDF_FILEOBJECT_CONFIG_INIT(&file, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WindropFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &file, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    WdfDeviceSetAlignmentRequirement(device, 4095);
    WDF_DMA_ENABLER_CONFIG_INIT(&dma, WdfDmaProfileScatterGatherDuplex, RADIO_DMA_BYTES);
    status = WdfDmaEnablerCreate(device, &dma, WDF_NO_OBJECT_ATTRIBUTES, &DeviceContext(device)->Dma);
    if (!NT_SUCCESS(status)) return status;
    status = WdfCommonBufferCreate(DeviceContext(device)->Dma, RADIO_DMA_BYTES,
        WDF_NO_OBJECT_ATTRIBUTES, &DeviceContext(device)->Buffer);
    if (!NT_SUCCESS(status)) return status;
    DeviceContext(device)->Slab = WdfCommonBufferGetAlignedVirtualAddress(DeviceContext(device)->Buffer);
    {
        PHYSICAL_ADDRESS address = WdfCommonBufferGetAlignedLogicalAddress(DeviceContext(device)->Buffer);
        if (address.QuadPart < 0 || (ULONGLONG)address.QuadPart + RADIO_DMA_BYTES > 0x100000000ULL)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        DeviceContext(device)->DmaBase = address.LowPart;
    }

    // The INF limits this interface to SYSTEM and administrators. This is a
    // function driver for this PCI device, not a filter on the Realtek driver.
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_WINDROP_RADIO, NULL);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = WindropIoControl;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS WindropPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    BUS_INTERFACE_STANDARD bus = {0};
    PCI_COMMON_CONFIG pci = {0};
    ULONG received;
    ULONGLONG bar2;
    ULONG index;
    NTSTATUS status;
    DEVICE_CONTEXT* context = DeviceContext(Device);

    RtlZeroMemory(&context->Info, sizeof(context->Info));
    status = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&bus, sizeof(bus), 1, NULL);
    if (!NT_SUCCESS(status)) return status;

    received = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &pci, 0, PCI_COMMON_HDR_LENGTH);
    bus.InterfaceDereference(bus.Context);
    if (received != PCI_COMMON_HDR_LENGTH) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (pci.VendorID != WINDROP_VENDOR_ID || pci.DeviceID != WINDROP_DEVICE_ID
        || (pci.HeaderType & ~PCI_MULTIFUNCTION) != PCI_DEVICE_TYPE)
        return STATUS_NOT_SUPPORTED;

    // BAR2 is the AX chip's 1 MiB register window, identified from PCI config,
    // never by assuming a memory descriptor's ordinal or accepting a user address.
    if ((pci.u.type0.BaseAddresses[2] & PCI_ADDRESS_IO_SPACE) != 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    bar2 = pci.u.type0.BaseAddresses[2] & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    if ((pci.u.type0.BaseAddresses[2] & PCI_ADDRESS_MEMORY_TYPE_MASK) == PCI_TYPE_64BIT)
        bar2 |= (ULONGLONG)pci.u.type0.BaseAddresses[3] << 32;
    if (bar2 == 0 || WdfCmResourceListGetCount(Raw) != WdfCmResourceListGetCount(Translated))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (index = 0; index < WdfCmResourceListGetCount(Raw); ++index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw = WdfCmResourceListGetDescriptor(Raw, index);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translated = WdfCmResourceListGetDescriptor(Translated, index);
        if (raw == NULL || translated == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (raw->Type != CmResourceTypeMemory || (ULONGLONG)raw->u.Memory.Start.QuadPart != bar2) continue;
        if (translated->Type != CmResourceTypeMemory
            || translated->u.Memory.Length < WINDROP_BAR2_MIN_BYTES)
            return STATUS_DEVICE_CONFIGURATION_ERROR;

        context->Info.Size = sizeof(context->Info);
        context->Info.Version = WINDROP_PROTOCOL_VERSION;
        context->Info.VendorId = pci.VendorID;
        context->Info.DeviceId = pci.DeviceID;
        context->Info.Bar2Bytes = translated->u.Memory.Length;
        context->Registers = MmMapIoSpace(translated->u.Memory.Start,
            translated->u.Memory.Length, MmNonCached);
        if (context->Registers == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        // PCI Command belongs to Windows. Only the OS's function-level reset
        // interface may recover a hung DMA engine; never write PCI headers.
        status = WdfFdoQueryForInterface(Device, &GUID_DEVICE_RESET_INTERFACE_STANDARD,
            (PINTERFACE)&context->Reset, sizeof(context->Reset), DEVICE_RESET_INTERFACE_VERSION_1, NULL);
        if (NT_SUCCESS(status) && context->Reset.DeviceReset != NULL &&
            (context->Reset.SupportedResetTypes & (1u << FunctionLevelDeviceReset))) {
            context->Info.Capabilities = WINDROP_CAP_REGISTERS | WINDROP_CAP_FRAME_DMA;
        } else {
            if (NT_SUCCESS(status) && context->Reset.InterfaceDereference != NULL)
                context->Reset.InterfaceDereference(context->Reset.Context);
            RtlZeroMemory(&context->Reset, sizeof(context->Reset));
        }
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

_Use_decl_annotations_
NTSTATUS WindropReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    DEVICE_CONTEXT* context = DeviceContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    // D0Exit already fenced DMA. Keep the common buffer allocated for the
    // device lifetime so power transitions never free a live descriptor slab.
    if (context->Registers != NULL) {
        MmUnmapIoSpace((PVOID)context->Registers, context->Info.Bar2Bytes);
        context->Registers = NULL;
    }
    if (context->Reset.InterfaceDereference != NULL) {
        context->Reset.InterfaceDereference(context->Reset.Context);
        RtlZeroMemory(&context->Reset, sizeof(context->Reset));
    }
    RtlZeroMemory(&context->Info, sizeof(context->Info));
    return STATUS_SUCCESS;
}

static VOID ResetComplete(NTSTATUS Status, PVOID Context)
{
    DEVICE_CONTEXT* context = Context;
    context->ResetStatus = Status;
    KeSetEvent(&context->ResetEvent, IO_NO_INCREMENT, FALSE);
}

// Vendor fail-stop code: never let a hung device DMA into reclaimed memory.
// Used only if an OS reset cannot establish safety, not for ordinary IO errors.
#define WINDROP_DMA_UNSAFE_BUGCHECK 0xE0574401u
static NTSTATUS ResetFunction(DEVICE_CONTEXT* context)
{
    NTSTATUS status;
    LARGE_INTEGER timeout;
    FUNCTION_LEVEL_DEVICE_RESET_PARAMETERS parameters;
    if (context->Reset.DeviceReset == NULL) return STATUS_NOT_SUPPORTED;
    KeInitializeEvent(&context->ResetEvent, NotificationEvent, FALSE);
    context->ResetStatus = STATUS_PENDING;
    RtlZeroMemory(&parameters, sizeof(parameters));
    parameters.Size = sizeof(parameters);
    parameters.DeviceResetCompletion = ResetComplete;
    parameters.CompletionContext = context;
    status = context->Reset.DeviceReset(context->Reset.Context, FunctionLevelDeviceReset, 0, &parameters);
    if (status == STATUS_PENDING) {
        timeout.QuadPart = -100000000LL; // 10 seconds; completion context stays device-owned
        if (KeWaitForSingleObject(&context->ResetEvent, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS)
            KeBugCheckEx(WINDROP_DMA_UNSAFE_BUGCHECK, 1, 0, 0, 0);
        status = context->ResetStatus;
    }
    if (NT_SUCCESS(status)) { context->Rings = FALSE; context->DmaPublished = FALSE; }
    return status;
}

static NTSTATUS FenceDma(DEVICE_CONTEXT* context)
{
    NTSTATUS status;
    if (!context->Touched || context->Removed) return STATUS_SUCCESS;
    status = RadioStop(context);
    if (!NT_SUCCESS(status)) status = ResetFunction(context);
    if (NT_SUCCESS(status)) context->Touched = FALSE;
    return status;
}

_Use_decl_annotations_
NTSTATUS WindropD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    UNREFERENCED_PARAMETER(Previous);
    DeviceContext(Device)->InD0 = TRUE;
    DeviceContext(Device)->Removed = FALSE;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS WindropD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Target);
    status = FenceDma(DeviceContext(Device));
    if (!NT_SUCCESS(status) && DeviceContext(Device)->DmaPublished)
        KeBugCheckEx(WINDROP_DMA_UNSAFE_BUGCHECK, 2, (ULONG_PTR)status, 0, 0);
    DeviceContext(Device)->InD0 = FALSE;
    return status;
}

_Use_decl_annotations_
VOID WindropFileCleanup(WDFFILEOBJECT File)
{
    WDFDEVICE device = WdfFileObjectGetDevice(File);
    DEVICE_CONTEXT* context = DeviceContext(device);
    if (context->InD0 && !NT_SUCCESS(FenceDma(context))) {
        if (context->DmaPublished)
            KeBugCheckEx(WINDROP_DMA_UNSAFE_BUGCHECK, 3, 0, 0, 0);
        WdfDeviceSetFailed(device, WdfDeviceFailedNoRestart);
    }
}

_Use_decl_annotations_
NTSTATUS WindropQueryRemove(WDFDEVICE Device)
{
    return NT_SUCCESS(FenceDma(DeviceContext(Device))) ? STATUS_SUCCESS : STATUS_DEVICE_HUNG;
}

_Use_decl_annotations_
VOID WindropSurpriseRemoval(WDFDEVICE Device)
{
    // The bus reports the function absent; avoid any further MMIO access.
    DeviceContext(Device)->Removed = TRUE;
    DeviceContext(Device)->Rings = FALSE;
    DeviceContext(Device)->DmaPublished = FALSE;
}

_Use_decl_annotations_
VOID WindropIoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
    size_t InputLength, ULONG Code)
{
    WINDROP_RADIO_INFO* output;
    DEVICE_CONTEXT* context = DeviceContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status;
    PVOID input = NULL;
    PVOID bytes = NULL;
    ULONG length = 0;
    if (context->Info.Size == 0 || context->Removed) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    if (Code == WINDROP_IOCTL_REGISTER) {
        WINDROP_REGISTER reg;
        if (InputLength != sizeof(reg) || OutputLength < sizeof(reg)) { status = STATUS_INVALID_BUFFER_SIZE; goto complete; }
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(reg), &input, NULL);
        if (!NT_SUCCESS(status)) goto complete;
        RtlCopyMemory(&reg, input, sizeof(reg));
        if ((reg.Width != 1 && reg.Width != 2 && reg.Width != 4) || reg.Write > 1 ||
            reg.Offset > context->Info.Bar2Bytes - reg.Width || (reg.Offset & (reg.Width - 1)) != 0 ||
            (reg.Write && RadioProtectedRegister(reg.Offset, reg.Width))) { status = STATUS_INVALID_PARAMETER; goto complete; }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(reg), &bytes, NULL);
        if (!NT_SUCCESS(status)) goto complete;
        if (reg.Write) {
            if (!context->Touched) {
                status = STATUS_DEVICE_NOT_READY;
                goto complete;
            }
            // Aggregate index clears are HAL initialization/deinit operations.
            // Permit them only with stopped, idle DMA, invalidating live rings.
            if ((reg.Offset < 0x1018 && reg.Offset + reg.Width > 0x1014) ||
                (reg.Offset < 0x11c8 && reg.Offset + reg.Width > 0x11c4) ||
                (reg.Offset < 0x1204 && reg.Offset + reg.Width > 0x1200)) {
                if (!NT_SUCCESS(RadioStop(context))) { status = STATUS_DEVICE_BUSY; goto complete; }
            }
            RadioWrite(context, reg.Offset, reg.Width, reg.Value);
        }
        else reg.Value = RadioRead(context, reg.Offset, reg.Width);
        RtlCopyMemory(bytes, &reg, sizeof(reg)); length = sizeof(reg);
        goto complete;
    }
    if (Code == WINDROP_IOCTL_TRANSMIT) {
        WINDROP_TX_HEADER header;
        if (InputLength < sizeof(header) || InputLength > sizeof(header) + WINDROP_TX_MAX || OutputLength != 0) {
            status = STATUS_INVALID_BUFFER_SIZE; goto complete;
        }
        status = WdfRequestRetrieveInputBuffer(Request, InputLength, &input, NULL);
        if (!NT_SUCCESS(status)) goto complete;
        RtlCopyMemory(&header, input, sizeof(header));
        if (header.Length != InputLength - sizeof(header)) { status = STATUS_INVALID_BUFFER_SIZE; goto complete; }
        status = RadioTransmit(context, header.Channel, (const UCHAR*)input + sizeof(header), header.Length);
        goto complete;
    }
    if (InputLength != 0) { status = STATUS_INVALID_PARAMETER; goto complete; }
    if (Code == WINDROP_IOCTL_INITIALIZE) {
        if (OutputLength != 0) { status = STATUS_INVALID_BUFFER_SIZE; goto complete; }
        if (context->Touched) { status = STATUS_DEVICE_BUSY; goto complete; }
        status = ResetFunction(context);
        if (NT_SUCCESS(status)) context->Touched = TRUE;
        goto complete;
    }
    if (Code == WINDROP_IOCTL_SETUP || Code == WINDROP_IOCTL_STOP) {
        if (OutputLength != 0) { status = STATUS_INVALID_BUFFER_SIZE; goto complete; }
        status = Code == WINDROP_IOCTL_SETUP ?
            (context->Touched ? RadioSetup(context) : STATUS_DEVICE_NOT_READY) : FenceDma(context);
        goto complete;
    }
    if (Code == WINDROP_IOCTL_RECEIVE) {
        status = WdfRequestRetrieveOutputBuffer(Request, WINDROP_FRAME_MAX, &bytes, NULL);
        if (NT_SUCCESS(status)) status = RadioReceive(context, bytes, &length);
        goto complete;
    }
    if (Code != WINDROP_IOCTL_GET_INFO) { status = STATUS_INVALID_DEVICE_REQUEST; goto complete; }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID*)&output, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    RtlCopyMemory(output, &context->Info, sizeof(*output));
    length = sizeof(*output);
complete:
    WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? length : 0);
}
