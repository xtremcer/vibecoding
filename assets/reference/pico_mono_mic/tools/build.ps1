<# build.ps1 - 用 ~/.pico-sdk 下自包含的 Pico SDK 2.1.1 工具链编译本固件。
# 依赖由 tools/install_deps.ps1 安装：SDK 2.1.1 + ARM GCC 14.2 + CMake 3.31.5 + Ninja 1.12.1
#   + pico-sdk-tools 2.1.1 (pioasm/elf2uf2) + picotool 2.1.1
# 用法： powershell -ExecutionPolicy Bypass -File tools\build.ps1
#>
$ErrorActionPreference = 'Stop'
$BASE = Join-Path $env:USERPROFILE '.pico-sdk'
$env:PICO_SDK_PATH = Join-Path $BASE 'sdk\2.1.1'
$env:Path = "$BASE\toolchain\14_2_Rel1\bin;$BASE\cmake\v3.31.5\bin;$BASE\ninja\v1.12.1;$env:Path"

$proj = Split-Path -Parent $PSScriptRoot
$bd = Join-Path $proj 'build211'
New-Item -ItemType Directory -Force -Path $bd | Out-Null
Push-Location $bd
cmake -G Ninja `
    "-Dpioasm_DIR=$BASE\tools-2.1.1\pico-sdk-tools\pioasm" `
    "-Dpicotool_DIR=$BASE\tools-2.1.1\picotool\picotool" `
    "-DCMAKE_MAKE_PROGRAM=$BASE\ninja\v1.12.1\ninja.exe" `
    ..
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "cmake configure failed" }
ninja
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "build failed" }
Pop-Location
Write-Host ""
Write-Host "OK. UF2: $bd\i2s_mic.uf2"
