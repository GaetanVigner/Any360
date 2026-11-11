<#
.SYNOPSIS
  Bytes of a running title's guest memory, as hex and text: what a guest address holds (a string a function
  compares against, a table, an object), read from the process without a debugger. The guest's memory starts at
  -HostBase in the host process (0x100000000 with the ReXGlue runtime: a crash report's "read of 0000000100000004"
  is guest address 4).
.EXAMPLE
  .\tools\guest_memory.ps1 -ProcessName simpsons -Address 0x82196A30,0x821892C0 -Length 96
#>
param([Parameter(Mandatory = $true)][string]$ProcessName, [Parameter(Mandatory = $true)][string[]]$Address, [int]$Length = 64,
      [string]$HostBase = '0x100000000')
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GuestMemory {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  public static byte[] Read(int pid, long address, int length) {
    IntPtr process = OpenProcess(0x0410, false, pid);
    if (process == IntPtr.Zero) return null;
    byte[] bytes = new byte[length];
    IntPtr read;
    bool ok = ReadProcessMemory(process, new IntPtr(address), bytes, new IntPtr(length), out read);
    CloseHandle(process);
    return ok ? bytes : null;
  }
}
'@
$p = Get-Process $ProcessName -ErrorAction Stop | Select-Object -First 1
$base = [Convert]::ToInt64(($HostBase -replace '^0x', ''), 16)
foreach ($a in $Address) {
  # An unquoted 0x82000000 reaches here as the negative number PowerShell made of it.
  $guest = if ($a -match '^-?\d+$') { [int64]$a -band 0xFFFFFFFFL } else { [Convert]::ToInt64(($a -replace '^0x', ''), 16) }
  $bytes = [GuestMemory]::Read($p.Id, $base + $guest, $Length)
  if (-not $bytes) { '{0:X8}: not readable' -f $guest; continue }
  for ($i = 0; $i -lt $bytes.Length; $i += 16) {
    $row = $bytes[$i..([Math]::Min($i + 15, $bytes.Length - 1))]
    $hex = ($row | ForEach-Object { $_.ToString('X2') }) -join ' '
    $text = -join ($row | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
    '{0:X8}: {1,-47}  {2}' -f ($guest + $i), $hex, $text
  }
}
