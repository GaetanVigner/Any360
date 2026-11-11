<#
.SYNOPSIS
  The names behind the addresses of a crash report: every "<module>+0x<offset>" of a title's log (or of the text
  given) whose module is the executable, by name and line (the PDB next to the exe). No debugger and no running
  process needed.
.EXAMPLE
  .\tools\crash_names.ps1 -Exe F:\title\out\title.exe -Log F:\title\out\logs\title_012.log
  .\tools\crash_names.ps1 -Exe F:\title\out\title.exe -Offsets 0x2B8C79D,0x2B90200
#>
param([Parameter(Mandatory = $true)][string]$Exe, [string]$Log = '', [string[]]$Offsets = @())
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CrashNames {
  [DllImport("dbghelp.dll")] static extern uint SymSetOptions(uint options);
  [DllImport("dbghelp.dll", CharSet = CharSet.Ansi)] static extern bool SymInitialize(IntPtr process, string path, bool invade);
  [DllImport("dbghelp.dll", CharSet = CharSet.Ansi)] static extern ulong SymLoadModuleEx(IntPtr process, IntPtr file, string image, string module, ulong at, uint size, IntPtr data, uint flags);
  [DllImport("dbghelp.dll")] static extern bool SymFromAddr(IntPtr process, ulong address, out ulong displacement, IntPtr symbol);
  [DllImport("dbghelp.dll")] static extern bool SymGetLineFromAddr64(IntPtr process, ulong address, out uint displacement, IntPtr line);
  [DllImport("dbghelp.dll")] static extern bool SymCleanup(IntPtr process);
  static readonly IntPtr process = new IntPtr(0x360);
  static IntPtr symbol, line;
  const ulong kBase = 0x10000000;
  public static bool Open(string exe, string folder) {
    SymSetOptions(0x2 | 0x10);
    symbol = Marshal.AllocHGlobal(88 + 1024);
    line = Marshal.AllocHGlobal(64);
    if (!SymInitialize(process, folder, false)) return false;
    return SymLoadModuleEx(process, IntPtr.Zero, exe, null, kBase, 0, IntPtr.Zero, 0) != 0;
  }
  public static void Close() { SymCleanup(process); }
  public static string Name(ulong offset) {
    for (int i = 0; i < 88 + 1024; i += 8) Marshal.WriteInt64(symbol, i, 0);
    Marshal.WriteInt32(symbol, 0, 88);
    Marshal.WriteInt32(symbol, 80, 1000);
    ulong displacement;
    if (!SymFromAddr(process, kBase + offset, out displacement, symbol)) return "?";
    string name = Marshal.PtrToStringAnsi(new IntPtr(symbol.ToInt64() + 84)) + "+0x" + displacement.ToString("X");
    for (int i = 0; i < 64; i += 8) Marshal.WriteInt64(line, i, 0);
    Marshal.WriteInt32(line, 0, 32);
    uint column;
    if (SymGetLineFromAddr64(process, kBase + offset, out column, line)) {
      name += "  (" + Marshal.PtrToStringAnsi(Marshal.ReadIntPtr(line, 16)) + ":" + Marshal.ReadInt32(line, 8) + ")";
    }
    return name;
  }
}
'@
$module = [System.IO.Path]::GetFileName($Exe)
if (-not [CrashNames]::Open($Exe, (Split-Path $Exe -Parent))) { 'cannot load the symbols of ' + $Exe; exit 1 }
if ($Log) {
  $seen = @{}
  foreach ($text in Get-Content $Log) {
    if ($text -notmatch 'crash report') { continue }
    $short = $text -replace '^.*crash report:\s*', ''
    $found = [regex]::Matches($short, [regex]::Escape($module) + '\+0x([0-9A-Fa-f]+)')
    if ($found.Count -eq 1) {
      "$short  $([CrashNames]::Name([Convert]::ToUInt64($found[0].Groups[1].Value, 16)))"
    } elseif ($found.Count) {
      # A line of several addresses (another thread's frames): each one by its name alone.
      [regex]::Replace($short, [regex]::Escape($module) + '\+0x([0-9A-Fa-f]+)', [System.Text.RegularExpressions.MatchEvaluator] {
        param($m) ([CrashNames]::Name([Convert]::ToUInt64($m.Groups[1].Value, 16)) -replace '\s+\(.*$', '') })
    } else { $short }
  }
}
foreach ($o in $Offsets) { "$o  $([CrashNames]::Name([Convert]::ToUInt64(($o -replace '^0x', ''), 16)))" }
[CrashNames]::Close()
