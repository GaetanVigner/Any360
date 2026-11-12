<#
.SYNOPSIS
  Where a running title's processor time goes, by sampling, with no debugger and nothing changed in the title:
  the threads that run are stopped about a thousand times a second each, the instruction each is at is noted, and
  the processor cycles the thread has used since its last sample (QueryThreadCycleTime) are put on it. A thread
  that waits uses no cycles and weighs nothing.

  The report gives the share of each thread, of each kind of code (the title's recompiled code by module of
  titles/<title>/modules.json, Any360's layers, the runtime, the GPU driver, Windows), the functions that cost
  most, and for the time spent in DLLs the function of the executable that called them.

  What it cannot tell: a function the compiler inlined into its caller is counted as its caller (the titles are
  built with link-time optimization: small functions of the recompiled code often are). The names come from the
  PDB next to the exe. A thread found at a wait or a sleep carries the cycles it used since its last sample: what
  it ran just before, and the call itself. A large share there is threads that wake and sleep again thousands of
  times a second (a wait with a short time-out in a loop), which the last table places by the function that asked.

  -LinesOf '<regex>': for the functions whose name matches, the cycles by source line too (from the PDB's line
  table), and the time their calls spent in DLLs (the GPU driver, waits) by the line that called. Where in a
  function the time goes, when the function itself is too big to tell.
.EXAMPLE
  .\tools\cpu_profile.ps1 -ProcessName simpsons -Title simpsons -Seconds 60 -Out profile.txt
.EXAMPLE
  .\tools\cpu_profile.ps1 -ProcessName crackdown -Seconds 45 -LinesOf 'VulkanRenderer::Draw' -Out draw.txt
#>
param([Parameter(Mandatory = $true)][string]$ProcessName, [string]$Title = '', [int]$Seconds = 60, [int]$Top = 40, [string]$Out = '',
      [string]$LinesOf = '')  # not $Lines: PowerShell's names ignore case, and $lines is the report
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
public static class CpuProfile {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, int tid);
  [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr thread);
  [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
  [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr thread, IntPtr context);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
  [DllImport("kernel32.dll")] static extern bool QueryThreadCycleTime(IntPtr thread, out ulong cycles);
  [DllImport("dbghelp.dll")] static extern uint SymSetOptions(uint options);
  [DllImport("dbghelp.dll", CharSet = CharSet.Ansi)] static extern bool SymInitialize(IntPtr process, string path, bool invade);
  [DllImport("dbghelp.dll")] static extern bool SymFromAddr(IntPtr process, ulong address, out ulong displacement, IntPtr symbol);
  [DllImport("dbghelp.dll")] static extern bool SymCleanup(IntPtr process);
  [DllImport("dbghelp.dll")] static extern bool SymGetLineFromAddr64(IntPtr process, ulong address, out uint displacement, IntPtr line);

  // By thread: instruction address -> cycles. And, for the cycles spent outside the executable, the first return
  // address into the executable found on the stack -> cycles.
  public static Dictionary<int, Dictionary<ulong, ulong>> ByThread = new Dictionary<int, Dictionary<ulong, ulong>>();
  public static Dictionary<ulong, ulong> Callers = new Dictionary<ulong, ulong>();
  public static long Samples = 0;
  static IntPtr process;

  static void Add(Dictionary<ulong, ulong> to, ulong key, ulong cycles) {
    ulong before;
    to.TryGetValue(key, out before);
    to[key] = before + cycles;
  }

  public static bool Run(int pid, int seconds, ulong low, ulong high, ulong code_low, ulong code_high) {
    // The class lives as long as the PowerShell session: nothing of an earlier run stays.
    ByThread.Clear();
    Callers.Clear();
    Samples = 0;
    process = OpenProcess(0x0410, false, pid);
    if (process == IntPtr.Zero) return false;
    IntPtr raw = Marshal.AllocHGlobal(1232 + 16);
    IntPtr context = new IntPtr((raw.ToInt64() + 15) & ~15L);
    byte[] stack = new byte[0x600];
    Dictionary<int, IntPtr> handles = new Dictionary<int, IntPtr>();
    Dictionary<int, ulong> at_refresh = new Dictionary<int, ulong>(), at_sample = new Dictionary<int, ulong>();
    List<int> active = new List<int>();
    Stopwatch clock = Stopwatch.StartNew();
    long next_refresh = 0, next_round = 0, round = Stopwatch.Frequency / 1000;
    while (clock.ElapsedMilliseconds < seconds * 1000L) {
      if (clock.ElapsedMilliseconds >= next_refresh) {
        // The threads that have run since the last look are the ones sampled until the next.
        next_refresh += 400;
        Process target;
        try { target = Process.GetProcessById(pid); } catch (ArgumentException) { break; }
        active.Clear();
        foreach (ProcessThread thread in target.Threads) {
          IntPtr handle;
          if (!handles.TryGetValue(thread.Id, out handle)) {
            handle = OpenThread(0x004A, false, thread.Id);
            if (handle == IntPtr.Zero) continue;
            handles[thread.Id] = handle;
          }
          ulong cycles, before;
          if (!QueryThreadCycleTime(handle, out cycles)) continue;
          bool known = at_refresh.TryGetValue(thread.Id, out before);
          at_refresh[thread.Id] = cycles;
          if (!known) { at_sample[thread.Id] = cycles; continue; }
          if (cycles - before > 400000) active.Add(thread.Id);
        }
        target.Dispose();
      }
      foreach (int tid in active) {
        IntPtr thread = handles[tid];
        for (int i = 0; i < 1232; i += 8) Marshal.WriteInt64(context, i, 0);
        Marshal.WriteInt32(context, 0x30, 0x100001);
        ulong rip = 0, caller = 0;
        if (SuspendThread(thread) == 0xFFFFFFFF) continue;
        if (GetThreadContext(thread, context)) {
          rip = (ulong)Marshal.ReadInt64(context, 0xF8);
          if (rip < low || rip >= high) {
            ulong rsp = (ulong)Marshal.ReadInt64(context, 0x98);
            IntPtr read;
            if (ReadProcessMemory(process, new IntPtr((long)rsp), stack, new IntPtr(stack.Length), out read)) {
              for (int i = 0; i + 8 <= read.ToInt64(); i += 8) {
                ulong value = BitConverter.ToUInt64(stack, i);
                if (value >= code_low && value < code_high) { caller = value; break; }
              }
            }
          }
        }
        ResumeThread(thread);
        ulong now, last;
        if (rip == 0 || !QueryThreadCycleTime(thread, out now)) continue;
        at_sample.TryGetValue(tid, out last);
        at_sample[tid] = now;
        ulong used = now > last ? now - last : 0;
        if (used == 0) continue;
        Dictionary<ulong, ulong> mine;
        if (!ByThread.TryGetValue(tid, out mine)) ByThread[tid] = mine = new Dictionary<ulong, ulong>();
        Add(mine, rip, used);
        if (caller != 0) Add(Callers, caller, used);
        ++Samples;
      }
      next_round += round;
      while (clock.ElapsedTicks < next_round) Thread.SpinWait(40);
    }
    foreach (IntPtr handle in handles.Values) CloseHandle(handle);
    Marshal.FreeHGlobal(raw);
    return true;
  }

  static IntPtr symbol;
  public static bool OpenSymbols(string path) {
    SymSetOptions(0x2 | 0x4 | 0x10);  // undecorated names, deferred loads, line numbers
    symbol = Marshal.AllocHGlobal(88 + 1024);
    line = Marshal.AllocHGlobal(64);
    return SymInitialize(process, path, true);
  }
  // The function an address is in, without the offset; null where no symbol is known.
  public static string Name(ulong address) {
    for (int i = 0; i < 88 + 1024; i += 8) Marshal.WriteInt64(symbol, i, 0);
    Marshal.WriteInt32(symbol, 0, 88);
    Marshal.WriteInt32(symbol, 80, 1000);
    ulong displacement;
    if (!SymFromAddr(process, address, out displacement, symbol)) return null;
    return Marshal.PtrToStringAnsi(new IntPtr(symbol.ToInt64() + 84));
  }
  static IntPtr line;
  // The source file (its name only) and line of an address; null where the PDB has none.
  public static string Line(ulong address) {
    for (int i = 0; i < 64; i += 8) Marshal.WriteInt64(line, i, 0);
    Marshal.WriteInt32(line, 0, 40);  // IMAGEHLP_LINE64
    uint displacement;
    if (!SymGetLineFromAddr64(process, address, out displacement, line)) return null;
    string file = Marshal.PtrToStringAnsi(Marshal.ReadIntPtr(line, 24));
    if (file == null) return null;
    int slash = Math.Max(file.LastIndexOf('\\'), file.LastIndexOf('/'));
    return file.Substring(slash + 1) + ":" + Marshal.ReadInt32(line, 16);
  }
  public static void Close() { SymCleanup(process); CloseHandle(process); }
}
'@
$p = Get-Process $ProcessName -ErrorAction Stop | Select-Object -First 1
$main = $p.MainModule
$low = [uint64]$main.BaseAddress.ToInt64(); $high = $low + [uint64]$main.ModuleMemorySize
$modules = @($p.Modules | ForEach-Object { [pscustomobject]@{ Low = [uint64]$_.BaseAddress.ToInt64(); High = [uint64]$_.BaseAddress.ToInt64() + [uint64]$_.ModuleMemorySize; Name = $_.ModuleName } })
# The executable's code section, so that a caller found on a stack is an address in code and not a pointer to its data.
$code_low = $low; $code_high = $high
try {
  $header = New-Object byte[] 4096
  $file = [System.IO.File]::OpenRead($main.FileName); [void]$file.Read($header, 0, 4096); $file.Close()
  $pe = [BitConverter]::ToInt32($header, 0x3C); $sections = [BitConverter]::ToUInt16($header, $pe + 6)
  $table = $pe + 24 + [BitConverter]::ToUInt16($header, $pe + 20)
  for ($i = 0; $i -lt $sections; $i++) {
    $at = $table + 40 * $i
    if ([System.Text.Encoding]::ASCII.GetString($header, $at, 5) -eq '.text') {
      $code_low = $low + [BitConverter]::ToUInt32($header, $at + 12); $code_high = $code_low + [BitConverter]::ToUInt32($header, $at + 8)
    }
  }
} catch { }
$cpu_before = $p.TotalProcessorTime.TotalSeconds; $started = Get-Date
if (-not [CpuProfile]::Run($p.Id, $Seconds, $low, $high, $code_low, $code_high)) { 'cannot open the process'; exit 1 }
$wall = ((Get-Date) - $started).TotalSeconds
$p.Refresh(); $cpu_used = if ($p.HasExited) { 0 } else { $p.TotalProcessorTime.TotalSeconds - $cpu_before }
if (-not [CpuProfile]::OpenSymbols((Split-Path $main.FileName -Parent))) { 'cannot load the symbols'; exit 1 }

# The title's modules, to place its recompiled functions (sub_XXXXXXXX).
$ranges = @()
if ($Title) {
  $map = Join-Path $PSScriptRoot "..\titles\$Title\modules.json"
  if (Test-Path $map) { $ranges = @((Get-Content $map -Raw | ConvertFrom-Json).modules | ForEach-Object { [pscustomobject]@{ Low = [Convert]::ToUInt32($_.start.Substring(2), 16); High = [Convert]::ToUInt32($_.end.Substring(2), 16); Name = $_.name; Kind = $_.kind } }) }
}
function Place([string]$name, [uint64]$address) {
  # -> the kind of code and the label of its group.
  if ($address -lt $low -or $address -ge $high) {
    $m = $modules | Where-Object { $address -ge $_.Low -and $address -lt $_.High } | Select-Object -First 1
    $dll = if ($m) { $m.Name } else { 'no module' }
    # A thread found at a wait or a sleep: the cycles are what it ran since its last sample plus the call itself.
    if ($name -match '^(Zw|Nt)(Wait|Delay|Alert|SignalAndWait|Yield|RemoveIoCompletion|UserGetMessage|UserMsgWait|UserPeekMessage)|^RtlDelayExecution$') { return 'waits and sleeps (system calls, with what ran just before them)', $dll }
    if ($dll -match '^rexruntime') { return 'the runtime (ReXGlue)', $dll }
    if ($dll -match '^nv|vulkan|^amd|^ig|dxgi|d3d') { return 'the GPU driver and Vulkan', $dll }
    if ($dll -match '^ntdll|^kernel|^win32u|^user32|^ucrtbase|^msvcp|^vcruntime') { return 'Windows and the C++ runtime DLLs', $dll }
    return 'other DLLs', $dll
  }
  if ($name -match '^(?:__imp__)?(?:guest_[a-z0-9]+_)?sub_([0-9A-F]{8})$') {
    $guest = [Convert]::ToUInt32($Matches[1], 16)
    $r = $ranges | Where-Object { $guest -ge $_.Low -and $guest -lt $_.High } | Select-Object -First 1
    if ($r) { return "the title's recompiled code", "$($r.Kind): $($r.Name)" }
    return "the title's recompiled code", 'no module named (game code)'
  }
  if ($name -match '^__(save|rest)(gpr|fpr|vmx)') { return "the title's recompiled code", 'register save and restore helpers' }
  if ($name -match 'any360::gfx') { return 'Any360', 'Any360 graphics (gfx)' }
  if ($name -match 'any360::sys|^any360_') { return 'Any360', 'Any360 system (sys)' }
  if ($name -match 'any360::xdk') { return 'Any360', 'Any360 native library functions (xdk)' }
  if ($name -match 'any360::') { return 'Any360', 'Any360 host binding' }
  if (-not $name) { return 'the executable, unnamed', 'no symbol' }
  return "the executable's other code", "host code of the title's port and its C++ runtime"
}

$by_name = @{}; $by_kind = @{}; $by_group = @{}; $by_thread = @{}; $total = [uint64]0; $by_line = @{}
foreach ($tid in [CpuProfile]::ByThread.Keys) {
  $mine = @{}; $sum = [uint64]0
  foreach ($pair in [CpuProfile]::ByThread[$tid].GetEnumerator()) {
    $name = [CpuProfile]::Name($pair.Key)
    $kind, $group = Place $name $pair.Key
    $label = if ($name) { $name } else { $group }
    if ($pair.Key -lt $low -or $pair.Key -ge $high) { $label = "$group!$label" }
    $by_name[$label] = [uint64]$by_name[$label] + $pair.Value
    if ($LinesOf -and $name -and $name -match $LinesOf) {
      $where = [CpuProfile]::Line($pair.Key); if (-not $where) { $where = 'no line' }
      $key = "$where  ($name)"; $by_line[$key] = [uint64]$by_line[$key] + $pair.Value
    }
    $by_kind[$kind] = [uint64]$by_kind[$kind] + $pair.Value
    $by_group["$kind | $group"] = [uint64]$by_group["$kind | $group"] + $pair.Value
    $mine[$label] = [uint64]$mine[$label] + $pair.Value
    $sum += $pair.Value
  }
  $by_thread[$tid] = [pscustomobject]@{ Cycles = $sum; Names = $mine }
  $total += $sum
}
$callers = @{}
$caller_lines = @{}
foreach ($pair in [CpuProfile]::Callers.GetEnumerator()) {
  $n = [CpuProfile]::Name($pair.Key); if (-not $n) { $n = 'no symbol' }; $callers[$n] = [uint64]$callers[$n] + $pair.Value
  if ($LinesOf -and $n -match $LinesOf) {
    $where = [CpuProfile]::Line($pair.Key); if (-not $where) { $where = 'no line' }
    $key = "$where  ($n)"; $caller_lines[$key] = [uint64]$caller_lines[$key] + $pair.Value
  }
}
[CpuProfile]::Close()
function Share([uint64]$cycles) { if ($total) { '{0,5:N1}%' -f (100.0 * $cycles / $total) } else { '  n/a' } }
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("$ProcessName, $([int]$wall) s sampled, $([CpuProfile]::Samples) samples; the process used $('{0:N2}' -f ($cpu_used / [Math]::Max($wall, 1))) processors on average")
$lines.Add(''); $lines.Add('by thread (its share of the cycles, then what it runs most):')
foreach ($t in ($by_thread.GetEnumerator() | Sort-Object { $_.Value.Cycles } -Descending | Select-Object -First 8)) {
  $top3 = ($t.Value.Names.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 3 | ForEach-Object { $n = $_.Key; if ($n.Length -gt 60) { $n = $n.Substring(0, 60) }; "$n $('{0:N0}' -f (100.0 * $_.Value / [Math]::Max($t.Value.Cycles, 1)))%" }) -join '; '
  $lines.Add("  $(Share $t.Value.Cycles)  thread $($t.Key): $top3")
}
$lines.Add(''); $lines.Add('by kind of code:')
foreach ($k in ($by_kind.GetEnumerator() | Sort-Object Value -Descending)) { $lines.Add("  $(Share $k.Value)  $($k.Key)") }
$lines.Add(''); $lines.Add('by module:')
foreach ($g in ($by_group.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 30)) { $lines.Add("  $(Share $g.Value)  $($g.Key)") }
$lines.Add(''); $lines.Add("the $Top functions that cost most:")
foreach ($n in ($by_name.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top)) {
  $label = $n.Key; $where = ''
  if ($label -match '^(?:__imp__)?(?:guest_[a-z0-9]+_)?sub_([0-9A-F]{8})$') { $guest = [Convert]::ToUInt32($Matches[1], 16); $r = $ranges | Where-Object { $guest -ge $_.Low -and $guest -lt $_.High } | Select-Object -First 1; $where = if ($r) { "  [$($r.Name)]" } else { '  [game code]' } }
  if ($label.Length -gt 110) { $label = $label.Substring(0, 110) }
  $lines.Add("  $(Share $n.Value)  $label$where")
}
$lines.Add(''); $lines.Add('time spent in DLLs (waits and sleeps among them), by the function of the executable that called them:')
foreach ($c in ($callers.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 15)) { $n = $c.Key; if ($n.Length -gt 110) { $n = $n.Substring(0, 110) }; $lines.Add("  $(Share $c.Value)  $n") }
if ($LinesOf) {
  $lines.Add(''); $lines.Add("by source line, the functions matching '$LinesOf' (their own code):")
  foreach ($l in ($by_line.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top)) { $n = $l.Key; if ($n.Length -gt 130) { $n = $n.Substring(0, 130) }; $lines.Add("  $(Share $l.Value)  $n") }
  $lines.Add(''); $lines.Add("by source line, the time those functions' calls spent in DLLs (the driver, waits):")
  foreach ($l in ($caller_lines.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top)) { $n = $l.Key; if ($n.Length -gt 130) { $n = $n.Substring(0, 130) }; $lines.Add("  $(Share $l.Value)  $n") }
}
if ($Out) {
  $lines | Set-Content -Path $Out -Encoding ascii
  # Every thread's cycles by function, to sort and group again without another run.
  $rows = New-Object System.Collections.Generic.List[string]
  $rows.Add('thread,cycles,function')
  foreach ($t in $by_thread.GetEnumerator()) { foreach ($n in $t.Value.Names.GetEnumerator()) { $rows.Add("$($t.Key),$($n.Value),`"$($n.Key -replace '`"', "'")`"") } }
  $rows | Set-Content -Path "$Out.csv" -Encoding ascii
  "written: $Out ($($lines.Count) lines) and $Out.csv ($($rows.Count - 1) rows)"
} else { $lines }
