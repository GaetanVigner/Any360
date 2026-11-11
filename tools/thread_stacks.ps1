<#
.SYNOPSIS
  Where the threads of a running title are, without a debugger: for each thread, the instruction it is at and the
  return addresses on its stack that fall in the title's executable, by name (the PDB next to the exe). The busiest
  thread is sampled several times first.

  For a title that stays black or stops answering: a thread that turns in the title's own code shows as the same
  callers under a changing top (The Simpsons Game read a level without end, one read failing each time round); one
  that waits shows what it waits in.

  The stack is not unwound: every value on it that is an address in the executable is listed, so a name may be a
  call made earlier whose address was left there. Read the names as the chain when they repeat from sample to sample.
.EXAMPLE
  .\tools\thread_stacks.ps1 -ProcessName simpsons -Out stacks.txt
#>
param([Parameter(Mandatory = $true)][string]$ProcessName, [int]$Frames = 14, [int]$Samples = 6, [string]$Out = '')
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Stacks {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, int tid);
  [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr thread);
  [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
  [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr thread, IntPtr context);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
  [DllImport("dbghelp.dll")] static extern uint SymSetOptions(uint options);
  [DllImport("dbghelp.dll", CharSet = CharSet.Ansi)] static extern bool SymInitialize(IntPtr process, string path, bool invade);
  [DllImport("dbghelp.dll")] static extern bool SymFromAddr(IntPtr process, ulong address, out ulong displacement, IntPtr symbol);
  [DllImport("dbghelp.dll")] static extern bool SymCleanup(IntPtr process);
  static IntPtr process, symbol;
  public static bool Open(int pid, string path) {
    process = OpenProcess(0x0410, false, pid);
    if (process == IntPtr.Zero) return false;
    SymSetOptions(0x2 | 0x4);
    symbol = Marshal.AllocHGlobal(88 + 1024);
    return SymInitialize(process, path, true);
  }
  public static void Close() { SymCleanup(process); CloseHandle(process); }
  public static string Name(ulong address) {
    for (int i = 0; i < 88 + 1024; i += 8) Marshal.WriteInt64(symbol, i, 0);
    Marshal.WriteInt32(symbol, 0, 88);
    Marshal.WriteInt32(symbol, 80, 1000);
    ulong displacement;
    if (!SymFromAddr(process, address, out displacement, symbol)) return null;
    return Marshal.PtrToStringAnsi(new IntPtr(symbol.ToInt64() + 84)) + "+0x" + displacement.ToString("X");
  }
  // rip, then the stack's values that are addresses in [low, high).
  public static ulong[] Thread(int tid, ulong low, ulong high, int frames) {
    IntPtr thread = OpenThread(0x004A, false, tid);
    if (thread == IntPtr.Zero) return new ulong[0];
    IntPtr raw = Marshal.AllocHGlobal(1232 + 16);
    IntPtr context = new IntPtr((raw.ToInt64() + 15) & ~15L);
    for (int i = 0; i < 1232; i += 8) Marshal.WriteInt64(context, i, 0);
    Marshal.WriteInt32(context, 0x30, 0x100003);
    var found = new List<ulong>();
    SuspendThread(thread);
    if (GetThreadContext(thread, context)) {
      ulong rsp = (ulong)Marshal.ReadInt64(context, 0x98);
      found.Add((ulong)Marshal.ReadInt64(context, 0xF8));
      byte[] stack = new byte[0x3000];
      IntPtr read;
      if (ReadProcessMemory(process, new IntPtr((long)rsp), stack, new IntPtr(stack.Length), out read)) {
        for (int i = 0; i + 8 <= read.ToInt64() && found.Count <= frames; i += 8) {
          ulong value = BitConverter.ToUInt64(stack, i);
          if (value >= low && value < high) found.Add(value);
        }
      }
    }
    ResumeThread(thread);
    CloseHandle(thread);
    Marshal.FreeHGlobal(raw);
    return found.ToArray();
  }
}
'@
$p = Get-Process $ProcessName -ErrorAction Stop | Select-Object -First 1
$main = $p.MainModule
$low = [uint64]$main.BaseAddress.ToInt64(); $high = $low + [uint64]$main.ModuleMemorySize
if (-not [Stacks]::Open($p.Id, (Split-Path $main.FileName -Parent))) { 'cannot open the process or its symbols'; exit 1 }
$lines = New-Object System.Collections.Generic.List[string]
$threads = $p.Threads | Sort-Object { $_.TotalProcessorTime } -Descending
$busy = $threads | Select-Object -First 1
$lines.Add("pid $($p.Id), $($threads.Count) threads; busiest tid $($busy.Id) ($([int]$busy.TotalProcessorTime.TotalSeconds) s of CPU), $Samples samples of it:")
for ($i = 0; $i -lt $Samples; $i++) {
  $a = [Stacks]::Thread($busy.Id, $low, $high, $Frames)
  $lines.Add('  ' + (($a | ForEach-Object { $n = [Stacks]::Name($_); if ($n) { $n } else { '0x{0:X}' -f $_ } }) -join ' < '))
  Start-Sleep -Milliseconds 150
}
foreach ($t in $threads) {
  $a = [Stacks]::Thread($t.Id, $low, $high, $Frames)
  if (-not $a.Count) { continue }
  $names = $a | ForEach-Object { $n = [Stacks]::Name($_); if ($n) { $n } else { '0x{0:X}' -f $_ } }
  $lines.Add("tid $($t.Id) cpu $([Math]::Round($t.TotalProcessorTime.TotalSeconds, 1)) s $($t.WaitReason): " + ($names -join ' < '))
}
[Stacks]::Close()
if ($Out) { $lines | Set-Content -Path $Out -Encoding ascii; "written: $Out ($($lines.Count) lines)" } else { $lines }
