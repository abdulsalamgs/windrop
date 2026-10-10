param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
if (-not ('WindropDevelopmentSignature' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
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
    [DllImport("wintrust.dll", ExactSpelling=true)]
    static extern int WinVerifyTrust(IntPtr window, ref Guid policy, ref TrustData data);
    public static int Verify(string path) {
        var file = new FileInfo { Size=(uint)Marshal.SizeOf(typeof(FileInfo)), Path=path };
        IntPtr pointer=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
        var policy=new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
        var data=new TrustData { Size=(uint)Marshal.SizeOf(typeof(TrustData)), Ui=2, Choice=1,
            File=pointer, Action=1, Flags=0x1010 }; // no UI, no revocation/network lookup
        try {
            Marshal.StructureToPtr(file,pointer,false);
            return WinVerifyTrust(new IntPtr(-1),ref policy,ref data);
        } finally {
            if(data.State!=IntPtr.Zero) { data.Action=2; WinVerifyTrust(new IntPtr(-1),ref policy,ref data); }
            Marshal.DestroyStructure(pointer,typeof(FileInfo)); Marshal.FreeHGlobal(pointer);
        }
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
Write-Host 'Verified package hashes and matching development signatures. This does not establish trusted provenance or authorize loading.'
