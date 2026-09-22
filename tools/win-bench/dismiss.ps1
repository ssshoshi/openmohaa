# dismiss.ps1 - OPTIONAL fallback. bench.sh no longer needs this: the CONTINUE card
# is now cleared in-engine by the `finishloadingscreen` console command (what the
# button's stuffcommand runs; see ui/loadingbar.txt). Keep this only for a build/menu
# where that command is unavailable - it foregrounds the window and injects key-downs
# via keybd_event (driver level, so SDL2 sees it regardless of WM_KEYDOWN vs raw input).
param([string]$ProcName = "openmohaa")

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue |
     Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $p) { Write-Output "dismiss: '$ProcName' not running"; exit 2 }

$def = @'
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
}
'@
Add-Type $def

$h = $p.MainWindowHandle
[Win]::ShowWindow($h, 9)       | Out-Null   # SW_RESTORE
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500

# SPACE, ENTER, SPACE - covers the CONTINUE card plus any follow-up prompt.
foreach ($vk in @(0x20, 0x0D, 0x20)) {
    [Win]::keybd_event([byte]$vk, 0, 0, [UIntPtr]::Zero)   # key down
    Start-Sleep -Milliseconds 60
    [Win]::keybd_event([byte]$vk, 0, 2, [UIntPtr]::Zero)   # key up (KEYEVENTF_KEYUP)
    Start-Sleep -Milliseconds 250
}
Write-Output "dismiss: sent SPACE/ENTER/SPACE to '$ProcName' (pid $($p.Id))"
