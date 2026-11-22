# Builds dist\Webbed-Coop-Setup.exe: the native DLL (zig cc, x86) and the PyInstaller installer.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force build | Out-Null

zig cc -target x86-windows-gnu -Os -s -shared src/WebbedCoop.c -o build/WebbedCoop.dll
if ($LASTEXITCODE -ne 0) { throw 'DLL build failed' }

python -m PyInstaller --noconfirm --clean --onefile --windowed `
    --name Webbed-Coop-Setup `
    --paths helper `
    --add-data 'installer/resources.delta;.' `
    --add-data 'build/WebbedCoop.dll;.' `
    --add-data 'licenses;licenses' `
    --distpath dist --workpath build/pyinstaller --specpath build `
    installer/installer.py
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }
Write-Host 'dist\Webbed-Coop-Setup.exe'
