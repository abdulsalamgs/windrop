#pragma once

// ABI v1: bounded capability query only. No addresses or kernel pointers cross
// this interface. NativeRadioDiagnostics.cs consumes this exact 24-byte layout.
DEFINE_GUID(GUID_DEVINTERFACE_WINDROP_RADIO,
    0x2e63236f, 0x48e1, 0x4a3a, 0xba, 0xf5, 0x38, 0x75, 0x99, 0xbf, 0x54, 0xa6);

#define WINDROP_IOCTL_GET_INFO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define WINDROP_PROTOCOL_VERSION 1
#define WINDROP_VENDOR_ID 0x10ec
#define WINDROP_DEVICE_ID 0xc852
#define WINDROP_BAR2_MIN_BYTES 0x100000

typedef struct _WINDROP_RADIO_INFO {
    ULONG Size;
    ULONG Version;
    ULONG VendorId;
    ULONG DeviceId;
    ULONG Bar2Bytes;
    ULONG Capabilities; // Zero until frame TX/RX and AWDL are implemented.
} WINDROP_RADIO_INFO;

C_ASSERT(sizeof(WINDROP_RADIO_INFO) == 24);
