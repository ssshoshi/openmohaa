# focus.ps1 - bring the running openmohaa window to the foreground.
#
# The r_gpuTimers report only averages *world* frames, and MOHAA throttles
# rendering when its window is unfocused/minimized - in the background the
# measurement window never accumulates enough frames to print. This just
# restores + foregrounds the window (no key input; the CONTINUE card is
# already cleared in-engine by `finishloadingscreen`).
param([string]$ProcName = "openmohaa")

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue |
     Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $p) { Write-Output "focus: '$ProcName' not running"; exit 2 }

$def = @'
using System;
using System.Runtime.InteropServices;
public class Fg {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
}
'@
Add-Type $def

$h = $p.MainWindowHandle
[Fg]::ShowWindow($h, 9)        | Out-Null   # SW_RESTORE
[Fg]::BringWindowToTop($h)     | Out-Null
[Fg]::SetForegroundWindow($h)  | Out-Null
Write-Output "focus: foregrounded '$ProcName' (pid $($p.Id))"
