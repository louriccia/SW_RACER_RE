param([string]$Codes = "1C", [int]$Gap = 2, [string]$Snap = "")
# Each code: hex scan code; prefix "E" for extended (arrows: E048 up, E050 down, E04B left, E04D right).
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Scan {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit, Size = 28)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(4)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public static void Hold(ushort scan, int ms) {
        INPUT[] down = { new INPUT { type = 1, ki = new KEYBDINPUT { wScan = scan, dwFlags = 0x0008 } } };
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(ms);
        INPUT[] up = { new INPUT { type = 1, ki = new KEYBDINPUT { wScan = scan, dwFlags = 0x000A } } };
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Press(ushort scan, bool ext) {
        uint f = 0x0008 | (ext ? 0x0001u : 0u);
        INPUT[] down = { new INPUT { type = 1, ki = new KEYBDINPUT { wScan = scan, dwFlags = f } } };
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(120);
        INPUT[] up = { new INPUT { type = 1, ki = new KEYBDINPUT { wScan = scan, dwFlags = f | 0x0002 } } };
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
$p = Get-Process swe1r_standalone -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -ne "" } | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
[void][Scan]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 300
foreach ($c in $Codes.Split(",")) {
    $ext = $c.StartsWith("E0") -and $c.Length -gt 2
    $code = [Convert]::ToUInt16($(if ($ext) { $c.Substring(2) } else { $c }), 16)
    [Scan]::Press($code, $ext)
    Start-Sleep $Gap
}
if ($Snap) { & "$PSScriptRoot\snap.ps1" -Name $Snap }
