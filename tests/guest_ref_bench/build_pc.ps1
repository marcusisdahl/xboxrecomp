# Builds and runs the guest-access micro-benchmark on the PC (x64 and x86,
# /O2 as the 360 builds generated code and /O1 as the PC reference build does).
#
#   -SrcDir    directory holding platform\recomp_guest_*.{h,c} (default: src)
#   -TypesDir  directory holding the recomp_types.h under test
#              (default: templates\runtime)
#   -OutDir    outputs (default: build\guest_ref_bench)
# Comparing two builds (say before/after a change): run once per -SrcDir /
# -TypesDir pair with different -OutDir values; the "sum" columns must match.
[CmdletBinding()]
param(
    [string] $SrcDir,
    [string] $TypesDir,
    [string] $OutDir
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$SrcDir) { $SrcDir = Join-Path $root 'src' }
if (!$TypesDir) { $TypesDir = Join-Path $root 'templates\runtime' }
if (!$OutDir) { $OutDir = Join-Path $root 'build\guest_ref_bench' }
$SrcDir = [IO.Path]::GetFullPath($SrcDir); $TypesDir = [IO.Path]::GetFullPath($TypesDir)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -property installationPath
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$src = Join-Path $PSScriptRoot 'guest_ref_bench.cpp'
$bus = Join-Path $SrcDir 'platform\recomp_guest_bus.c'
$atomic = Join-Path $SrcDir 'platform\recomp_guest_atomic_win.cpp'
$failed = 0
foreach ($arch in @('x64', 'x86')) {
    foreach ($opt in @('O2', 'O1')) {
        $name = "guest_ref_bench_${arch}_$opt"
        $exe = Join-Path $OutDir "$name.exe"
        $objDir = Join-Path $OutDir "$name.obj"
        New-Item -ItemType Directory -Force $objDir | Out-Null
        $cmd = "`"$vcvars`" $arch >nul && cl /nologo /TP /EHsc /W3 /$opt /I`"$TypesDir`" /I`"$SrcDir`" /I`"$SrcDir\platform`" /Fo`"$objDir\\`" /Fe`"$exe`" `"$src`" `"$bus`" `"$atomic`""
        $ErrorActionPreference = 'Continue'
        cmd /c $cmd 2>&1 | Out-File -Encoding utf8 (Join-Path $OutDir "$name.build.log")
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0 -or !(Test-Path $exe)) { Write-Output "BUILD FAILED $name (see $name.build.log)"; $failed++; continue }
        & $exe (Join-Path $OutDir "$name.txt") | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Output "RUN FAILED $name"; $failed++; continue }
        Write-Output "== $name"
        Get-Content (Join-Path $OutDir "$name.txt") | Where-Object { $_ -like 'bench *' }
    }
}
if ($failed) { throw "$failed benchmark build/run problems" }
