param([string]$Process = "swe1r_standalone", [int]$Samples = 20)
# Peak audio level of every audio session belonging to $Process (Windows Core Audio session meter).
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator { int NotImpl1(); [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice ppDevice); }
[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice { [PreserveSig] int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams, [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface); }
[ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2 { int NotImpl1(); int NotImpl2(); [PreserveSig] int GetSessionEnumerator(out IAudioSessionEnumerator SessionEnum); }
[ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator { [PreserveSig] int GetCount(out int SessionCount); [PreserveSig] int GetSession(int SessionCount, out IAudioSessionControl2 Session); }
[ComImport, Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2 { int a(); int b(); int c(); int d(); int e(); int f(); int g(); int h(); int i(); int j(); int k(); [PreserveSig] int GetProcessId(out uint pRetVal); }
[ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioMeterInformation { [PreserveSig] int GetPeakValue(out float pfPeak); }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorComObject { }
public static class Peak {
    public static string Read(int pid) {
        var en = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
        IMMDevice dev; en.GetDefaultAudioEndpoint(0, 1, out dev);
        Guid iid = typeof(IAudioSessionManager2).GUID; object o;
        dev.Activate(ref iid, 23, IntPtr.Zero, out o);
        var mgr = (IAudioSessionManager2)o; IAudioSessionEnumerator se; mgr.GetSessionEnumerator(out se);
        int n; se.GetCount(out n); string r = "";
        for (int i = 0; i < n; i++) {
            IAudioSessionControl2 s; se.GetSession(i, out s); uint p; s.GetProcessId(out p);
            if (p != pid) continue;
            float pk; ((IAudioMeterInformation)s).GetPeakValue(out pk);
            r += pk.ToString("0.000") + " ";
        }
        return r == "" ? "no session" : r;
    }
}
"@
$p = Get-Process $Process | Where-Object { $_.MainWindowTitle -ne "" } | Select-Object -First 1
$max = 0.0
for ($i = 0; $i -lt $Samples; $i++) {
    $v = [Peak]::Read($p.Id)
    foreach ($x in $v.Trim().Split(" ")) { $f = 0.0; if ([float]::TryParse($x, [ref]$f) -and $f -gt $max) { $max = $f } }
    Start-Sleep -Milliseconds 150
}
"pid $($p.Id) last: $v  max peak over $Samples samples: $max"
