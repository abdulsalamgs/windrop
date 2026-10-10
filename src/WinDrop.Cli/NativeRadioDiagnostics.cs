using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace WinDrop.Cli;

// CLI diagnostics use the read-only capability query. The hardware worker owns
// the writable frame interface. Keep this in step with the driver ABI.
internal static class NativeRadioDiagnostics
{
    private static readonly Guid InterfaceId = new("2e63236f-48e1-4a3a-baf5-387599bf54a6");
    private const uint GetInfo = 0x00226000; // UNKNOWN, function 0x800, BUFFERED, READ_ACCESS

    internal sealed record Result(bool Ready, string Backend, string Reason,
        bool DriverLoaded = false, string? VendorId = null, string? DeviceId = null,
        uint? Bar2Bytes = null);

    internal static Result Check()
    {
        if (!OperatingSystem.IsWindows())
            return Unavailable("The native PCIe radio driver requires Windows.");

        Guid id = InterfaceId;
        using var devices = SetupDiGetClassDevsW(ref id, null, IntPtr.Zero, 0x12); // PRESENT | DEVICEINTERFACE
        if (devices.IsInvalid) return Failure("enumerating Windows radio drivers");

        var data = new DeviceInterfaceData { Size = (uint)Marshal.SizeOf<DeviceInterfaceData>() };
        if (!SetupDiEnumDeviceInterfaces(devices, IntPtr.Zero, ref id, 0, ref data))
        {
            int error = Marshal.GetLastWin32Error();
            return error == 259 // NO_MORE_ITEMS
                ? Unavailable("The Windrop Windows PCIe radio driver is not installed or its device has not started. See docs/WINDOWS-DRIVER.md.")
                : Failure("enumerating Windows radio drivers", error);
        }

        if (SetupDiGetDeviceInterfaceDetailW(devices, ref data, IntPtr.Zero, 0, out uint needed, IntPtr.Zero)
            || Marshal.GetLastWin32Error() != 122 || needed < 8 || needed > 65536)
            return Unavailable("The Windows driver returned an invalid device interface.");

        IntPtr detail = Marshal.AllocHGlobal((int)needed);
        try
        {
            // cbSize includes alignment padding on x64; DevicePath still starts at byte 4.
            Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
            if (!SetupDiGetDeviceInterfaceDetailW(devices, ref data, detail, needed, out _, IntPtr.Zero))
                return Failure("reading the Windows driver interface");
            string path = Marshal.PtrToStringUni(detail + 4, (int)(needed - 4) / 2)!.Split('\0')[0];
            if (path.Length == 0) return Unavailable("The Windows driver returned an empty device interface.");

            using var device = CreateFileW(path, 0x80000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (device.IsInvalid) return Failure("opening the Windows driver (administrator access and an idle driver are required)");
            if (!DeviceIoControl(device, GetInfo, IntPtr.Zero, 0, out DriverInfo info,
                    (uint)Marshal.SizeOf<DriverInfo>(), out uint read, IntPtr.Zero))
                return Failure("querying Windows radio capabilities");
            if (read != 24 || info.Size != 24 || info.Version != 2
                || info.VendorId != 0x10ec || info.DeviceId != 0xc852 || info.Bar2Bytes < 0x100000)
                return Unavailable("The Windows driver reported an incompatible identity or protocol.");

            // Driver presence establishes transport capabilities only. The regular
            // doctor command checks the running authenticated AWDL bridge.
            return new(false, "windows-pcie",
                (info.Capabilities & 3) == 3
                    ? "The Windows RTL8852CE frame driver is loaded. Start scripts/start-native.ps1 to run AWDL, then use windrop doctor to check the bridge. Hardware interoperability remains unqualified."
                    : "The Windows RTL8852CE driver lacks register/frame DMA capabilities. Rebuild the native package.",
                true, "10ec", "c852", info.Bar2Bytes);
        }
        finally { Marshal.FreeHGlobal(detail); }
    }

    private static Result Unavailable(string reason) => new(false, "windows-pcie", reason);
    private static Result Failure(string action, int? error = null)
    {
        int code = error ?? Marshal.GetLastWin32Error();
        return Unavailable($"Failed {action}: Windows error {code} ({new Win32Exception(code).Message}).");
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DriverInfo
    {
        public uint Size, Version, VendorId, DeviceId, Bar2Bytes, Capabilities;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInterfaceData
    {
        public uint Size;
        public Guid ClassId;
        public uint Flags;
        public UIntPtr Reserved;
    }

    private sealed class DeviceInfoSet : SafeHandleZeroOrMinusOneIsInvalid
    {
        private DeviceInfoSet() : base(true) { }
        protected override bool ReleaseHandle() => SetupDiDestroyDeviceInfoList(handle);
    }

    [DllImport("setupapi.dll", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern DeviceInfoSet SetupDiGetClassDevsW(ref Guid classId, string? enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInterfaces(DeviceInfoSet devices, IntPtr device, ref Guid classId,
        uint index, ref DeviceInterfaceData data);
    [DllImport("setupapi.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet devices, ref DeviceInterfaceData data,
        IntPtr detail, uint size, out uint required, IntPtr device);
    [DllImport("setupapi.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr devices);
    [DllImport("kernel32.dll", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security,
        uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code, IntPtr input, uint inputSize,
        out DriverInfo output, uint outputSize, out uint returned, IntPtr overlapped);
}
