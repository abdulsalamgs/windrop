#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#include "protocol.h"

typedef struct _DEVICE_CONTEXT {
    WINDROP_RADIO_INFO Info;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD WindropDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE WindropPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE WindropReleaseHardware;
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
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware = WindropPrepareHardware;
    power.EvtDeviceReleaseHardware = WindropReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &power);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

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
        // No MMIO mapping, register writes, bus-master changes, DMA, or radio
        // initialization in this increment. Driver presence is not AirDrop readiness.
        context->Info.Capabilities = 0;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

_Use_decl_annotations_
NTSTATUS WindropReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    RtlZeroMemory(&DeviceContext(Device)->Info, sizeof(WINDROP_RADIO_INFO));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID WindropIoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
    size_t InputLength, ULONG Code)
{
    WINDROP_RADIO_INFO* output;
    DEVICE_CONTEXT* context = DeviceContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status;
    UNREFERENCED_PARAMETER(OutputLength);

    if (Code != WINDROP_IOCTL_GET_INFO) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    if (InputLength != 0) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (context->Info.Size == 0) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID*)&output, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    RtlCopyMemory(output, &context->Info, sizeof(*output));
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*output));
}
