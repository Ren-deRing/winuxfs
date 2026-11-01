# SPDX-License-Identifier: GPL-2.0-only
# XFS acceptance, VM side (M2 design 10.7). Run in the VM with Windows
# PowerShell from a directory holding winuxfs.exe and
# winuxfs_backend_xfs.dll:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File xfs-vm-acceptance.ps1 `
#       -Dir C:\t15 -Out C:\t15\out
#
# The data disk (the only non-boot, non-system disk) carries a GPT whose
# partitions are built by `tools/xfs_vm_acceptance.py prepare`:
#   1 clean image   2 image with a dirty log   3 hole/unwritten image   4 perf
# Everything happens in this one process tree because a drive letter exists
# only in the logon session of the mounting process (docs/vm.md).
#
# Output files in -Out: manifest-{main,dirty,hole,perf}.json (same schema as
# tools/xfs_manifest.py; only what Windows can see: kind, size, sha256, with
# names reversed through the winuxfs name mapping back to surrogateescape
# style), dirty-refusal.txt and results.json. The host compares them with
# `tools/xfs_vm_acceptance.py verify`.
param(
  [string]$Dir = (Split-Path -Parent $MyInvocation.MyCommand.Path),
  [string]$Out = (Join-Path $Dir 'out'),
  [string]$Letter = 'Y:',
  [int]$PartMain = 1, [int]$PartDirty = 2, [int]$PartHole = 3, [int]$PartPerf = 4,
  [long]$SkipHashOver = 1073741824   # hole image only: far.bin is 1 TiB (hash "skipped")
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Dir 'winuxfs.exe'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -ErrorAction SilentlyContinue | Remove-Item -Force
$results = [ordered]@{}
$script:mountMs = [ordered]@{}
$sw = [System.Diagnostics.Stopwatch]

# ---- the data disk -------------------------------------------------------
$disks = @(Get-Disk | Where-Object { -not $_.IsBoot -and -not $_.IsSystem })
if ($disks.Count -ne 1) { throw "expected exactly one data disk, found $($disks.Count)" }
$dev = "disk:$($disks[0].Number)"
$results.disk = $dev

# ---- mount / unmount -----------------------------------------------------
function Start-XfsMount([int]$Part, [string]$Options = '', [string]$Tag = 'm') {
  $o = Join-Path $Out "mount-$Tag.out.txt"; $e = Join-Path $Out "mount-$Tag.err.txt"
  $a = @('mount', $Letter, $dev, '--type', 'xfs', '--partition', "$Part", '--plugin-dir', $Dir)
  if ($Options) { $a += @('--options', $Options) }
  $t = $sw::StartNew()
  $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden `
         -RedirectStandardOutput $o -RedirectStandardError $e
  $null = $p.Handle   # without this ExitCode stays null
  $up = $false
  while ($t.Elapsed.TotalSeconds -lt 60) {
    if (Test-Path -LiteralPath "$Letter\") { $up = $true; break }
    if ($p.HasExited) { break }
    Start-Sleep -Milliseconds 20
  }
  $ms = $t.Elapsed.TotalMilliseconds
  $script:mountMs[$Tag] = [math]::Round($ms)
  if (-not $up) { $p.WaitForExit(10000) | Out-Null }
  [pscustomobject]@{ Proc = $p; Up = $up; Ms = $ms; OutFile = $o; ErrFile = $e }
}
function Stop-XfsMount($m) {
  if (-not $m.Up) { return }
  & $exe unmount $Letter | Out-Null
  $m.Proc.WaitForExit(30000) | Out-Null
  if (-not $m.Proc.HasExited) { $m.Proc.Kill() }
  $t = $sw::StartNew()
  while ((Test-Path -LiteralPath "$Letter\") -and $t.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 100 }
}

# ---- manifest walk -------------------------------------------------------
# Reverse of crates/winuxfs-core/src/namemap.rs, then JSON-escape the way
# Python's json.dumps does (invalid disk byte -> lone surrogate U+DCxx).
function ConvertTo-DiskName([string]$s) {
  $sb = New-Object System.Text.StringBuilder
  $i = 0
  while ($i -lt $s.Length) {
    $c = [int]$s[$i]
    if ($c -eq 0xF8FF -and $i + 1 -lt $s.Length) { $i++; [void]$sb.Append($s[$i]) }
    elseif ($c -ge 0xF000 -and $c -le 0xF0FF) { [void]$sb.Append([char]($c -band 0xFF)) }
    elseif ($c -ge 0xF700 -and $c -le 0xF7FF) { [void]$sb.Append([char](0xDC00 + ($c -band 0xFF))) }
    else { [void]$sb.Append([char]$c) }
    $i++
  }
  $sb.ToString()
}
function ConvertTo-JsonString([string]$s) {
  $sb = New-Object System.Text.StringBuilder
  [void]$sb.Append('"')
  foreach ($ch in $s.ToCharArray()) {
    $u = [int]$ch
    if ($u -ge 0x20 -and $u -lt 0x7F -and $ch -ne '"' -and $ch -ne '\') { [void]$sb.Append($ch) }
    else { [void]$sb.AppendFormat('\u{0:x4}', $u) }
  }
  [void]$sb.Append('"')
  $sb.ToString()
}
function Get-Sha256([string]$path) {
  if ($path.Length -lt 240) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
  }
  # Long paths: the cmdlet is subject to MAX_PATH; hash through a \\?\ stream.
  $fs = [System.IO.File]::OpenRead('\\?\' + $path)
  try {
    $h = [System.Security.Cryptography.SHA256]::Create()
    return ([BitConverter]::ToString($h.ComputeHash($fs)) -replace '-', '').ToLower()
  } finally { $fs.Dispose() }
}
# Writes the manifest of $Letter\ to $file. Returns @{Entries; Errors}.
function Write-Manifest([string]$file, [long]$Skip = [long]::MaxValue) {
  $w = New-Object System.IO.StreamWriter($file, $false, (New-Object System.Text.UTF8Encoding($false)))
  $errors = New-Object System.Collections.Generic.List[string]
  $script:count = 0
  $w.Write("{`n `"entries`": {")
  $first = $true
  $emit = {
    param($rel, $body)
    if (-not $script:first) { $w.Write(',') }
    $script:first = $false
    $w.Write("`n  " + (ConvertTo-JsonString $rel) + ': ' + $body)
    $script:count++
  }
  $script:first = $true
  & $emit '/' '{"kind": "dir"}'
  $stack = New-Object System.Collections.Generic.Stack[object]
  $stack.Push(@('', "$Letter\"))
  while ($stack.Count -gt 0) {
    $cur = $stack.Pop(); $relBase = $cur[0]; $abs = $cur[1]
    $di = New-Object System.IO.DirectoryInfo('\\?\' + $abs)
    $items = @($di.EnumerateFileSystemInfos() | Sort-Object { $_.Name })
    foreach ($it in $items) {
      $rel = $relBase + '/' + (ConvertTo-DiskName $it.Name)
      $child = $abs.TrimEnd('\') + '\' + $it.Name
      if ($it.Attributes -band [System.IO.FileAttributes]::Directory) {
        & $emit $rel '{"kind": "dir"}'
        $stack.Push(@($rel, $child))
      } else {
        $size = $it.Length
        try {
          if ($size -gt $Skip) { $sha = 'skipped' } else { $sha = Get-Sha256 $child }
        } catch { $sha = 'error: ' + $_.Exception.Message; $errors.Add("$rel : $sha") }
        & $emit $rel ('{"kind": "file", "sha256": ' + (ConvertTo-JsonString $sha) + ', "size": ' + $size + '}')
      }
    }
  }
  $w.Write("`n },`n `"version`": 1`n}`n")
  $w.Close()
  @{ Entries = $script:count; Errors = $errors }
}

# ---- 1. clean image: mount, walk, probes ---------------------------------
$m = Start-XfsMount $PartMain 'ro' 'main'
$results.main_mounted = $m.Up
$results.first_mount_ms = [math]::Round($m.Ms)
if (-not $m.Up) {
  $results.main_stderr = (Get-Content $m.ErrFile -Raw)
} else {
  $t = $sw::StartNew()
  $null = Get-ChildItem -LiteralPath "$Letter\" -Force
  $results.first_root_list_ms = [math]::Round($t.Elapsed.TotalMilliseconds)

  # volume flags and write refusal
  $vi = (& fsutil fsinfo volumeinfo $Letter) -join "`n"
  $results.volume_info = $vi
  $results.volume_read_only = ($vi -match 'Is ReadOnly|Read Only Volume')
  $ro = Get-Item -LiteralPath "$Letter\modes\readonly.txt"
  $rw = Get-Item -LiteralPath "$Letter\modes\private.txt"
  $results.attr_readonly_file_is_readonly = [bool]$ro.IsReadOnly
  $results.attr_rw_file_is_readonly = [bool]$rw.IsReadOnly
  $refused = [ordered]@{}
  $tries = [ordered]@{
    overwrite = { Set-Content -LiteralPath "$Letter\tiny.txt" -Value x -ErrorAction Stop }
    append    = { Add-Content -LiteralPath "$Letter\tiny.txt" -Value x -ErrorAction Stop }
    create    = { New-Item -ItemType File -Path "$Letter\newfile.txt" -ErrorAction Stop | Out-Null }
    mkdir     = { New-Item -ItemType Directory -Path "$Letter\newdir" -ErrorAction Stop | Out-Null }
    delete    = { Remove-Item -LiteralPath "$Letter\tiny.txt" -Force -ErrorAction Stop }
    rename    = { Rename-Item -LiteralPath "$Letter\tiny.txt" -NewName renamed.txt -ErrorAction Stop }
    setattr   = { Set-ItemProperty -LiteralPath "$Letter\tiny.txt" -Name IsReadOnly -Value $true -ErrorAction Stop }
  }
  foreach ($k in $tries.Keys) {
    try { & $tries[$k]; $refused[$k] = 'ALLOWED' } catch { $refused[$k] = $_.Exception.Message }
  }
  $results.write_attempts = $refused
  $results.tiny_unchanged = ((Get-Content -LiteralPath "$Letter\tiny.txt" -Raw) -eq "hello xfs`n") -and
                            (-not (Test-Path -LiteralPath "$Letter\newfile.txt")) -and
                            (-not (Test-Path -LiteralPath "$Letter\newdir"))

  # 3000-entry directory (a small listing first, so .NET's own JIT time is
  # not counted in the cold number)
  $null = [System.IO.Directory]::GetFileSystemEntries("$Letter\dir_block")
  $t = $sw::StartNew()
  $names = [System.IO.Directory]::GetFileSystemEntries("$Letter\many")
  $results.many_count = $names.Count
  $results.many_unique = @($names | Select-Object -Unique).Count
  $results.many_enum_ms = [math]::Round($t.Elapsed.TotalMilliseconds, 1)
  $t = $sw::StartNew()
  $items = @(Get-ChildItem -LiteralPath "$Letter\many" -Force)
  $results.many_get_childitem_ms = [math]::Round($t.Elapsed.TotalMilliseconds, 1)
  $results.many_get_childitem_count = $items.Count
  # a second, warm listing
  $t = $sw::StartNew()
  $null = [System.IO.Directory]::GetFileSystemEntries("$Letter\many")
  $results.many_enum_warm_ms = [math]::Round($t.Elapsed.TotalMilliseconds, 1)

  # symlinks (shown as files whose size is the target length) and fifo
  $sym = [ordered]@{}
  foreach ($n in 'links_local', 'links_remote_300', 'links_dangling', 'fifo') {
    $p = "$Letter\$n"
    try {
      $i = Get-Item -LiteralPath $p -Force
      $e = [ordered]@{ attributes = "$($i.Attributes)"; length = $(if ($i.PSIsContainer) { -1 } else { $i.Length }) }
      if (-not $i.PSIsContainer -and $i.Length -gt 0) {
        try { $e.content = [System.IO.File]::ReadAllText($p) } catch { $e.read_error = $_.Exception.Message }
      }
      $sym[$n] = $e
    } catch { $sym[$n] = [ordered]@{ error = $_.Exception.Message } }
  }
  $results.symlinks = $sym

  $t = $sw::StartNew()
  $r = Write-Manifest (Join-Path $Out 'manifest-main.json')
  $results.main_walk_ms = [math]::Round($t.Elapsed.TotalMilliseconds)
  $results.main_entries = $r.Entries
  $results.main_hash_errors = @($r.Errors)
  Stop-XfsMount $m
}

# ---- 2. dirty log: refused with a message --------------------------------
$m = Start-XfsMount $PartDirty 'ro' 'dirty'
$results.dirty_mounted = $m.Up
$results.dirty_exit = $(if ($m.Proc.HasExited) { $m.Proc.ExitCode } else { -1 })
$msg = (Get-Content $m.ErrFile -Raw) + (Get-Content $m.OutFile -Raw)
[System.IO.File]::WriteAllText((Join-Path $Out 'dirty-refusal.txt'), $msg, (New-Object System.Text.UTF8Encoding($false)))
$results.dirty_message = $msg
if ($m.Up) { Stop-XfsMount $m }

# ---- 3. same image with norecovery ---------------------------------------
$m = Start-XfsMount $PartDirty 'ro,norecovery' 'norec'
$results.norecovery_mounted = $m.Up
if ($m.Up) {
  $r = Write-Manifest (Join-Path $Out 'manifest-dirty.json')
  $results.dirty_entries = $r.Entries
  $results.dirty_hash_errors = @($r.Errors)
  Stop-XfsMount $m
} else { $results.norecovery_stderr = (Get-Content $m.ErrFile -Raw) }

# ---- 4. hole / unwritten image --------------------------------------------
$m = Start-XfsMount $PartHole 'ro' 'hole'
$results.hole_mounted = $m.Up
if ($m.Up) {
  $r = Write-Manifest (Join-Path $Out 'manifest-hole.json') $SkipHashOver
  $results.hole_hash_errors = @($r.Errors)
  Stop-XfsMount $m
} else { $results.hole_stderr = (Get-Content $m.ErrFile -Raw) }

# ---- 5. perf image: sequential read throughput ----------------------------
$m = Start-XfsMount $PartPerf 'ro' 'perf'
$results.perf_mounted = $m.Up
if ($m.Up) {
  $f = "$Letter\big.bin"
  $len = (Get-Item -LiteralPath $f).Length
  $buf = New-Object byte[] (1MB)
  $fs = New-Object System.IO.FileStream($f, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read, 1048576, [System.IO.FileOptions]::SequentialScan)
  $t = $sw::StartNew(); $total = 0L
  while (($n = $fs.Read($buf, 0, $buf.Length)) -gt 0) { $total += $n }
  $sec = $t.Elapsed.TotalSeconds
  $fs.Dispose()
  $results.seqread_bytes = $total
  $results.seqread_mib_s = [math]::Round($total / 1MB / $sec, 1)
  $r = Write-Manifest (Join-Path $Out 'manifest-perf.json')
  $results.perf_hash_errors = @($r.Errors)
  Stop-XfsMount $m
}

$results.mount_ms = $script:mountMs
[System.IO.File]::WriteAllText((Join-Path $Out 'results.json'), ($results | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
Write-Output 'xfs-vm-acceptance: done'
