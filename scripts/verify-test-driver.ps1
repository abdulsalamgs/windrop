param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
if (-not ('WindropDevelopmentSignature' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using Microsoft.Win32.SafeHandles;
using System.Runtime.InteropServices;
public static class WindropDevelopmentSignature {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct FileInfo { public uint Size; [MarshalAs(UnmanagedType.LPWStr)] public string Path; public IntPtr Handle, Subject; }
    [StructLayout(LayoutKind.Sequential)]
    struct TrustData {
        public uint Size; public IntPtr Policy, Sip;
        public uint Ui, Revocation, Choice; public IntPtr File;
        public uint Action; public IntPtr State, Url; public uint Flags, Context;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct CatalogInfo {
        public uint Size, Version;
        [MarshalAs(UnmanagedType.LPWStr)] public string Catalog, Tag, Path;
        public IntPtr Handle, Hash; public uint HashSize;
        public IntPtr Context, Admin;
    }
    [DllImport("wintrust.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool CryptCATAdminAcquireContext2(out IntPtr context, IntPtr subsystem, string algorithm, IntPtr policy, uint flags);
    [DllImport("wintrust.dll", SetLastError=true)]
    static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr context, SafeFileHandle file, ref uint size, byte[] hash, uint flags);
    [DllImport("wintrust.dll")]
    static extern bool CryptCATAdminReleaseContext(IntPtr context, uint flags);
    [DllImport("wintrust.dll", ExactSpelling=true)]
    static extern int WinVerifyTrust(IntPtr window, ref Guid policy, ref TrustData data);
    public static int Verify(string path) {
        var file = new FileInfo { Size=(uint)Marshal.SizeOf(typeof(FileInfo)), Path=path };
        IntPtr pointer=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
        var policy=new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
        var data=new TrustData { Size=(uint)Marshal.SizeOf(typeof(TrustData)), Ui=2, Choice=1,
            File=pointer, Action=1, Flags=0x1010 }; // no UI, no revocation/network lookup
        bool marshaled=false;
        try {
            Marshal.StructureToPtr(file,pointer,false);
            marshaled=true;
            return WinVerifyTrust(new IntPtr(-1),ref policy,ref data);
        } finally {
            if(data.State!=IntPtr.Zero) { data.Action=2; WinVerifyTrust(new IntPtr(-1),ref policy,ref data); }
            if(marshaled) Marshal.DestroyStructure(pointer,typeof(FileInfo)); Marshal.FreeHGlobal(pointer);
        }
    }
    public static int VerifyMember(string catalog, string path) {
        IntPtr admin;
        if(!CryptCATAdminAcquireContext2(out admin,IntPtr.Zero,"SHA256",IntPtr.Zero,0))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            using(var file=File.OpenRead(path)) {
                uint size=0;
                if(!CryptCATAdminCalcHashFromFileHandle2(admin,file.SafeFileHandle,ref size,null,0))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                var hash=new byte[size];
                if(!CryptCATAdminCalcHashFromFileHandle2(admin,file.SafeFileHandle,ref size,hash,0))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                var member=new CatalogInfo { Size=(uint)Marshal.SizeOf(typeof(CatalogInfo)),
                    Catalog=catalog, Tag=BitConverter.ToString(hash).Replace("-",""), Path=path,
                    Handle=file.SafeFileHandle.DangerousGetHandle(), Admin=admin };
                IntPtr pointer=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(CatalogInfo)));
                var policy=new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
                var data=new TrustData { Size=(uint)Marshal.SizeOf(typeof(TrustData)), Ui=2,
                    Choice=2, File=pointer, Action=1, Flags=0x1010 };
                bool marshaled=false;
                try {
                    Marshal.StructureToPtr(member,pointer,false); marshaled=true;
                    return WinVerifyTrust(new IntPtr(-1),ref policy,ref data);
                } finally {
                    if(data.State!=IntPtr.Zero) { data.Action=2; WinVerifyTrust(new IntPtr(-1),ref policy,ref data); }
                    if(marshaled) Marshal.DestroyStructure(pointer,typeof(CatalogInfo));
                    Marshal.FreeHGlobal(pointer);
                }
            }
        } finally { CryptCATAdminReleaseContext(admin,0); }
    }
}
'@
}
$root = (Resolve-Path -LiteralPath $PackageDirectory).Path
$state = Get-Content -LiteralPath (Join-Path $root 'package.json') -Raw | ConvertFrom-Json
if ($state.schema -ne 1 -or $state.purpose -ne 'local-development-only') { throw 'Unsupported test-driver package' }
foreach ($relative in @('driver/WindropRadio.inf','driver/WindropRadio.sys','driver/WindropRadio.cat','WindropDevelopment.cer')) {
    $actual = (Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash
    if ($actual -ne $state.sha256.$relative) { throw "Package hash mismatch: $relative" }
}
$certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $root 'WindropDevelopment.cer'))
if ($certificate.Thumbprint -ne $state.certificateThumbprint -or $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) {
    throw 'Invalid or expired public development certificate'
}
foreach ($name in @('WindropRadio.sys','WindropRadio.cat')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $root "driver/$name")
    $result = [WindropDevelopmentSignature]::Verify((Join-Path $root "driver/$name"))
    # Accept only success or CERT_E_UNTRUSTEDROOT for this exact development
    # signer. PowerShell can label that expected root failure UnknownError.
    if ($result -notin @(0,-2146762487) -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
        throw "Invalid development signature: $name ($($signature.Status))"
    }
}
foreach ($name in @('WindropRadio.inf','WindropRadio.sys')) {
    $result = [WindropDevelopmentSignature]::VerifyMember((Join-Path $root 'driver/WindropRadio.cat'),(Join-Path $root "driver/$name"))
    if ($result -notin @(0,-2146762487)) { throw "Invalid signed catalog member: $name ($result)" }
}
Write-Host 'Verified package hashes and matching development signatures. This does not establish trusted provenance or authorize loading.'
