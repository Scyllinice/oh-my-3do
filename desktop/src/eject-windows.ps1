# Eject only the selected card/media, never its USB reader or hub.
param([Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z]$')][string]$Letter)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$logical = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($Letter):'"
if ($null -eq $logical) { throw 'Drive not found' }
if ($logical.DriveType -ne 2 -or $logical.DeviceID -eq $env:SystemDrive) {
    throw 'This drive does not expose removable media. Use Windows Safely Remove Hardware; removing its device may affect other volumes.'
}
# A card can itself contain several volumes. Do not remove media containing
# another mounted volume, even though other slots in the reader are independent.
$partitions = @(Get-CimAssociatedInstance -InputObject $logical -Association Win32_LogicalDiskToPartition)
$disks = @($partitions | ForEach-Object {
    Get-CimAssociatedInstance -InputObject $_ -Association Win32_DiskDriveToDiskPartition
} | Sort-Object DeviceID -Unique)
if ($disks.Count -ne 1) { throw 'Cannot identify the selected card. Use Windows Safely Remove Hardware.' }
$volumes = @(Get-CimAssociatedInstance -InputObject $disks[0] -Association Win32_DiskDriveToDiskPartition | ForEach-Object {
    Get-CimAssociatedInstance -InputObject $_ -Association Win32_LogicalDiskToPartition
} | Sort-Object DeviceID -Unique)
if ($volumes.Count -ne 1 -or $volumes[0].DeviceID -ne $logical.DeviceID) {
    throw 'Other mounted volumes share this card. Use Windows Safely Remove Hardware.'
}
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class OhMy3DOEject {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputSize, IntPtr output, uint outputSize, out uint returned, IntPtr overlapped);
    public static void RemoveMedia(Func<uint, int> control) {
        // A successful lock flushes cached writes and proves no files are open.
        int error = control(0x90018); // FSCTL_LOCK_VOLUME
        if (error != 0) throw new Win32Exception(error, "The card is in use or Windows denied exclusive access. Close files and try again.");
        try {
            error = control(0x90020); // FSCTL_DISMOUNT_VOLUME: only after lock succeeds
            if (error != 0) throw new Win32Exception(error, "Windows could not dismount the selected card.");
            error = control(0x2d4808); // IOCTL_STORAGE_EJECT_MEDIA: selected media, not PnP reader
            if (error != 0) throw new Win32Exception(error, "The reader could not eject the selected card. Use Windows eject controls.");
        } finally { control(0x9001c); } // FSCTL_UNLOCK_VOLUME; handle close also releases lock
    }
    public static int Eject(char letter) {
        if (!Char.IsLetter(letter) || letter > 127) throw new ArgumentException("Invalid drive letter");
        using (var handle = CreateFileW(@"\\.\" + letter + ":", 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (handle.IsInvalid) return Marshal.GetLastWin32Error();
            RemoveMedia(delegate(uint code) {
                uint returned;
                return DeviceIoControl(handle, code, IntPtr.Zero, 0, IntPtr.Zero, 0, out returned, IntPtr.Zero) ? 0 : Marshal.GetLastWin32Error();
            });
            return 0;
        }
    }
}
'@
$result = [OhMy3DOEject]::Eject([char]$Letter)
if ($result -eq 5) {
    # Standard-user direct volume access may be denied. Ask Explorer to eject
    # this volume using its normal media action. The caller verifies removal.
    $shell = New-Object -ComObject Shell.Application
    $item = $shell.NameSpace(17).ParseName("$($Letter):")
    if ($null -eq $item) { throw 'Windows could not find this card for eject.' }
    $item.InvokeVerb('Eject')
} elseif ($result -ne 0) {
    throw "Cannot open the selected card for eject (Windows error $result). Use Windows eject controls."
}
