# Builds and runs the MMX/SSE lane test on the PC (Windows, Visual Studio).
#
#   x86 explicit-bus  -- also checks every case against the CPU's own MMX/SSE
#   x86 flat          -- the desktop memory model, also checked against the CPU
#   x64 explicit-bus  -- no MMX intrinsics on x64; hashes only
#   x64 flat
#   x86/x64 shift     -- explicit bus with RECOMP_MMX_SHIFT_LANES=1 and
#                        RECOMP_XMM_SCALAR_PROXY=1, the helper variants the
#                        big-endian build uses, checked against the CPU on x86
#                        like the others
# All six must print identical case hashes; the x86 explicit-bus output is the
# golden file the big-endian (Xbox 360) run is compared against
# (compare.py). Outputs go to -OutDir (default build\simd_lanes).
[CmdletBinding()]
param(
    [string] $OutDir,
    # Build against another copy of recomp_types.h (a directory holding one).
    [string] $TypesDir
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$OutDir) { $OutDir = Join-Path $root 'build\simd_lanes' }
if (!$TypesDir) { $TypesDir = Join-Path $root 'templates\runtime' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -property installationPath
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$src = Join-Path $PSScriptRoot 'simd_lanes.cpp'
$common = "/nologo /EHsc /O2 /W3 /DSIMD_LANES_MAIN /I`"$TypesDir`" /I`"$root\src`" /I`"$root\src\platform`""
$bus = "`"$root\src\platform\recomp_guest_bus.c`""
$failed = 0
foreach ($arch in @('x86', 'x64')) {
    foreach ($mode in @('explicit', 'flat', 'shift')) {
        $name = "simd_lanes_${arch}_$mode"
        $exe = Join-Path $OutDir "$name.exe"
        $objDir = Join-Path $OutDir "$name.obj"
        New-Item -ItemType Directory -Force $objDir | Out-Null
        $defs = if ($mode -eq 'flat') { '/DSIMD_LANES_FLAT' } elseif ($mode -eq 'shift') { '/DRECOMP_MMX_SHIFT_LANES=1 /DRECOMP_XMM_SCALAR_PROXY=1' } else { '' }
        $extra = if ($mode -eq 'flat') { '' } else { $bus }
        $cmd = "`"$vcvars`" $arch >nul && cl $common $defs /Fo`"$objDir\\`" /Fe`"$exe`" `"$src`" $extra"
        $ErrorActionPreference = 'Continue'
        cmd /c $cmd 2>&1 | Out-File -Encoding utf8 (Join-Path $OutDir "$name.build.log")
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0 -or !(Test-Path $exe)) { Write-Output "BUILD FAILED $name (see $name.build.log)"; $failed++; continue }
        & $exe (Join-Path $OutDir "$name.bin") | Out-File -Encoding ascii (Join-Path $OutDir "$name.txt")
        $rc = $LASTEXITCODE
        $sum = Select-String -Path (Join-Path $OutDir "$name.txt") -Pattern '^simd_lanes: \d+ cases' | Select-Object -First 1
        Write-Output "$name exit=$rc $($sum.Line)"
        if ($rc -ne 0) { $failed++ }
    }
}
$hashes = @{}
foreach ($f in Get-ChildItem (Join-Path $OutDir 'simd_lanes_*.txt')) {
    $hashes[$f.BaseName] = (Get-Content $f | Where-Object { $_ -like 'case *' } | ForEach-Object { ($_ -split '\s+')[0..2] -join ' ' }) -join "`n"
}
$golden = $hashes['simd_lanes_x86_explicit']
if (!$golden) { throw 'no golden output (x86 explicit build failed)' }
foreach ($k in $hashes.Keys) {
    if ($hashes[$k] -ne $golden) { Write-Output "HASH MISMATCH: $k differs from simd_lanes_x86_explicit"; $failed++ }
}
if ($failed) { throw "$failed lane-test problems" }
Write-Output "All PC builds agree; golden: $OutDir\simd_lanes_x86_explicit.txt"
