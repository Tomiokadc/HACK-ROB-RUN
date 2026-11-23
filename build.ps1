param([switch]$Test)

$ErrorActionPreference = 'Stop'

$compiler = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\*\BuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe' |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (!$compiler) { throw 'MSVC Build Tools are required.' }

$toolRoot = (Get-Item -LiteralPath $compiler).Directory.Parent.Parent.Parent.FullName
$kits = 'C:\Program Files (x86)\Windows Kits\10'
$sdk = Get-ChildItem (Join-Path $kits 'Include') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'km\ntifs.h') } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
if (!$sdk) { throw 'The Windows Driver Kit (km headers) is required.' }

$name = if ($Test) { 'RobRunProbeTest' } else { 'RobRunProbe' }
$out = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force $out | Out-Null
$object = Join-Path $out ($name + '.obj')
$binary = Join-Path $out ($name + '.sys')

$options = @(
    '/nologo', '/c', '/TC', '/kernel', '/O2', '/GS', '/guard:cf', '/Zl', '/W4', '/WX',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000008'
)
if ($Test) { $options += '/DINJECT_TEST_DEVICE' }
foreach ($dir in @('km', 'km/crt', 'shared', 'ucrt')) {
    $options += '/I' + (Join-Path $kits "Include/$sdk/$dir")
}
$options += '/I' + (Join-Path $toolRoot 'include')
$options += '/I' + (Join-Path $PSScriptRoot 'src')
$options += @((Join-Path $PSScriptRoot 'src/inject_driver.c'), ('/Fo' + $object))

& $compiler @options
if ($LASTEXITCODE -ne 0) { throw 'RobRunProbe.sys compilation failed.' }

$linker = Join-Path (Split-Path $compiler -Parent) 'link.exe'
$lib = Join-Path $kits "Lib/$sdk/km/x64"
& $linker /nologo /driver /subsystem:native,10.00 /machine:x64 /entry:GsDriverEntry `
    /WX /nodefaultlib /incremental:no /opt:ref /dynamicbase /nxcompat /integritycheck `
    /guard:cf /release ('/out:' + $binary) $object `
    (Join-Path $lib 'ntoskrnl.lib') (Join-Path $lib 'hal.lib') `
    (Join-Path $lib 'wdmsec.lib') (Join-Path $lib 'BufferOverflowK.lib')
if ($LASTEXITCODE -ne 0) { throw 'RobRunProbe.sys link failed.' }

Write-Output $binary
