#pragma once

// ABI v2: buffered register and frame operations. No DMA addresses or kernel
// pointers cross this interface. Every operation requires an exclusive admin handle.
DEFINE_GUID(GUID_DEVINTERFACE_WINDROP_RADIO,
    0x2e63236f, 0x48e1, 0x4a3a, 0xba, 0xf5, 0x38, 0x75, 0x99, 0xbf, 0x54, 0xa6);

#define WINDROP_IOCTL_GET_INFO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define WINDROP_IOCTL_REGISTER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define WINDROP_IOCTL_SETUP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define WINDROP_IOCTL_TRANSMIT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define WINDROP_IOCTL_RECEIVE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define WINDROP_IOCTL_STOP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define WINDROP_PROTOCOL_VERSION 2
#define WINDROP_CAP_REGISTERS 1
#define WINDROP_CAP_FRAME_DMA 2
#define WINDROP_FRAME_MAX 32768
#define WINDROP_TX_MAX 4152
#define WINDROP_VENDOR_ID 0x10ec
#define WINDROP_DEVICE_ID 0xc852
#define WINDROP_BAR2_MIN_BYTES 0x100000

typedef struct _WINDROP_RADIO_INFO {
    ULONG Size;
    ULONG Version;
    ULONG VendorId;
    ULONG DeviceId;
    ULONG Bar2Bytes;
    ULONG Capabilities; // Transport capabilities, never AWDL/AirDrop readiness.
} WINDROP_RADIO_INFO;

C_ASSERT(sizeof(WINDROP_RADIO_INFO) == 24);

typedef struct _WINDROP_REGISTER {
    ULONG Offset;
    ULONG Width; // 1, 2 or 4; naturally aligned within BAR2.
    ULONG Write; // 0 = read, 1 = write.
    ULONG Value;
} WINDROP_REGISTER;

typedef struct _WINDROP_TX_HEADER {
    ULONG Channel; // AX DMA channel 0..12, including firmware command channel 12.
    ULONG Length; // Descriptor + payload bytes follow this header.
} WINDROP_TX_HEADER;
