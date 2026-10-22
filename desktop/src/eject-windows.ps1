# The caller supplies one validated drive letter. Request PnP safe removal;
# never force a dismount, disable a device, or eject an ancestor/hub.
param([Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z]$')][string]$Letter)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$logical = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($Letter):'"
if ($null -eq $logical) { throw 'Drive not found' }
$partitions = @(Get-CimAssociatedInstance -InputObject $logical -Association Win32_LogicalDiskToPartition)
$disks = @($partitions | ForEach-Object {
    Get-CimAssociatedInstance -InputObject $_ -Association Win32_DiskDriveToDiskPartition
} | Sort-Object DeviceID -Unique)
if ($disks.Count -ne 1) { throw 'Cannot identify one physical drive. Use Windows Safely Remove Hardware.' }
$disk = $disks[0]
if ($logical.DriveType -ne 2 -and $disk.InterfaceType -ne 'USB' -and $disk.PNPDeviceID -notlike 'USBSTOR\*') {
    throw 'This is not a removable or USB drive. Use Windows Safely Remove Hardware.'
}
$volumes = @(Get-CimAssociatedInstance -InputObject $disk -Association Win32_DiskDriveToDiskPartition | ForEach-Object {
    Get-CimAssociatedInstance -InputObject $_ -Association Win32_LogicalDiskToPartition
} | Sort-Object DeviceID -Unique)
if ($volumes.Count -ne 1 -or $volumes[0].DeviceID -ne $logical.DeviceID -or $logical.DeviceID -eq $env:SystemDrive) {
    throw 'Other volumes share this drive. Use Windows Safely Remove Hardware.'
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OhMy3DOEject {
    [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
    public static extern uint CM_Locate_DevNodeW(out uint node, string id, uint flags);
    [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
    public static extern uint CM_Request_Device_EjectW(uint node, out uint veto, StringBuilder name, uint length, uint flags);
}
'@
[uint32]$node = 0
$result = [OhMy3DOEject]::CM_Locate_DevNodeW([ref]$node, $disk.PNPDeviceID, 0)
if ($result -ne 0) { throw "Cannot locate the drive device (Configuration Manager code $result)." }
[uint32]$veto = 0
$name = [Text.StringBuilder]::new(512)
$result = [OhMy3DOEject]::CM_Request_Device_EjectW($node, [ref]$veto, $name, 512, 0)
if ($result -ne 0) {
    $reasons = @('Unknown reason', 'A legacy device prevents removal', 'A handle is waiting to close', 'A program is using the drive', 'A service is using the drive', 'A file or device handle is open', 'Another device prevents removal', 'A driver refused removal', 'The removal request is invalid', 'The driver has insufficient power', 'The device cannot be disabled', 'A legacy driver prevents removal', 'Windows requires additional permissions', 'The device was already removed')
    $reason = if ($veto -lt $reasons.Length) { $reasons[$veto] } else { 'Windows refused removal' }
    throw "$reason. $name (Configuration Manager code $result, veto $veto). Close files and use Windows Safely Remove Hardware if this continues."
}
