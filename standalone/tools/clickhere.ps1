param([int]$Clicks = 1)
# Left click at the current position, without moving the cursor.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Click {
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
$p = Get-Process swe1r_standalone -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -ne "" } | Select-Object -First 1
if ($p) { [void][Click]::SetForegroundWindow($p.MainWindowHandle) }
Start-Sleep -Milliseconds 300
for ($i = 0; $i -lt $Clicks; $i++) {
    [Click]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    [Click]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
}
