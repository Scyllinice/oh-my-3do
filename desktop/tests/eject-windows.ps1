# Windows-only tests. Compile production declarations; use simulated control
# results, without opening, locking, dismounting or ejecting a real volume.
$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '../src/eject-windows.ps1')
$tokens = $null; $errors = $null
$null = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$native = [regex]::Match($source, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@").Groups[1].Value
Add-Type -TypeDefinition $native
function Check($fail, $expected) {
    $calls = [Collections.Generic.List[uint32]]::new()
    $control = [Func[uint32,int]] { param($code) $calls.Add($code); if ($code -eq $fail) { return 5 }; return 0 }
    $failed = $false
    try { [OhMy3DOEject]::RemoveMedia($control) } catch { $failed = $true }
    if ($failed -ne ($fail -ne 0)) { throw 'Unexpected removal result' }
    if (($calls -join ',') -ne ($expected -join ',')) { throw "Wrong control order: $calls" }
}
Check 0 @(0x90018,0x90020,0x2d4808,0x9001c)
Check 0x90018 @(0x90018) # Busy: never dismount or eject
Check 0x90020 @(0x90018,0x90020,0x9001c)
Check 0x2d4808 @(0x90018,0x90020,0x2d4808,0x9001c)
if ($source -match 'CM_Request_Device_Eject|CM_Get_Parent') { throw 'Must not remove the reader' }
Write-Output 'Windows selected-media eject tests passed (no devices ejected).'
