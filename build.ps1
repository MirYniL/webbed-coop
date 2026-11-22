# Builds dist\Webbed-Coop-Setup.exe.
#   zig         - C compiler for the game extension (x86) and the native installer (x64)
#   -Python     - a CPython 3.11 x64 folder from python.org (its signed runtime ships with the mod)
param([string]$Python = "$env:LOCALAPPDATA\Python\pythoncore-3.11-64")
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force build | Out-Null

zig cc -target x86-windows-gnu -Os -s -shared src/WebbedCoop.c -o build/WebbedCoop.dll
if ($LASTEXITCODE -ne 0) { throw 'Game extension build failed' }

zig cc -target x86_64-windows-gnu -Os -s -municode '-Wl,--subsystem,windows' `
    installer/setup.c installer/setup.rc -o build/setup-stub.exe -lbcrypt -lshell32 -ladvapi32 -luser32
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }

& "$Python\python.exe" -I tools/pack.py build/setup-stub.exe build/WebbedCoop.dll $Python dist/Webbed-Coop-Setup.exe
if ($LASTEXITCODE -ne 0) { throw 'Packing failed' }
