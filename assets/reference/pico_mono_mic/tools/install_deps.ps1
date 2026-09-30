<# install_deps.ps1
# 在本机构建一份「自包含、可复用」的 Pico SDK 2.1.1 全链路编译依赖。
# 目标目录对齐 VS Code Pico 扩展约定： $env:USERPROFILE\.pico-sdk\
#   sdk\2.1.1            (Pico SDK，需含 tinyusb 子模块；可用 git clone --recursive -b 2.1.1)
#   toolchain\14_2_Rel1   (ARM GNU Toolchain 14.2.Rel1)
#   cmake\v3.31.5        (CMake)
#   ninja\v1.12.1        (Ninja)
#   picotool\2.1.1       (picotool，可选)
#
# 下载策略（按用户要求）：直连 -> 国内镜像(ghproxy/tuna) -> 系统代理
# 用法： powershell -ExecutionPolicy Bypass -File install_deps.ps1
#>

$ErrorActionPreference = 'Stop'
$base = Join-Path $env:USERPROFILE '.pico-sdk'
New-Item -ItemType Directory -Force -Path $base | Out-Null

# 系统代理（若已设置则自动使用）
$proxy = if ($env:https_proxy) { $env:https_proxy } else { $null }

# 各依赖： 直连URL , 国内镜像URL , 解压后的顶层目录名 , 安装目标子目录 , 校验文件
$deps = @(
  @{
    Name = 'cmake'
    Primary = 'https://github.com/Kitware/CMake/releases/download/v3.31.5/cmake-3.31.5-windows-x86_64.zip'
    Mirror  = 'https://mirrors.tuna.tsinghua.edu.cn/github-release/Kitware/CMake/v3.31.5/cmake-3.31.5-windows-x86_64.zip'
    TopDir  = 'cmake-3.31.5-windows-x86_64'
    Dest    = 'cmake\v3.31.5'
    Check   = 'bin\cmake.exe'
  },
  @{
    Name = 'arm-gcc'
    Primary = 'https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-mingw-w64-i686-arm-none-eabi.zip'
    Mirror  = 'https://ghproxy.com/https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-mingw-w64-i686-arm-none-eabi.zip'
    TopDir  = 'arm-gnu-toolchain-14.2.rel1-mingw-w64-i686-arm-none-eabi'
    Dest    = 'toolchain\14_2_Rel1'
    Check   = 'bin\arm-none-eabi-gcc.exe'
  },
  @{
    Name = 'ninja'
    Primary = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip'
    Mirror  = 'https://mirrors.tuna.tsinghua.edu.cn/github-release/ninja-build/ninja/v1.12.1/ninja-win.zip'
    TopDir  = ''   # 压缩包内直接是 ninja.exe
    Dest    = 'ninja\v1.12.1'
    Check   = 'ninja.exe'
  },
  @{
    # pioasm 2.1.1 + elf2uf2（SDK 生成 .pio.h 必需，且要 2.1.1 版才支持 -v 参数）
    Name = 'pico-sdk-tools'
    Primary = 'https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.1.1-3/pico-sdk-tools-2.1.1-x64-win.zip'
    Mirror  = 'https://ghproxy.com/https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.1.1-3/pico-sdk-tools-2.1.1-x64-win.zip'
    TopDir  = ''            # 顶层即 pioasm\ 目录
    Dest    = 'tools-2.1.1\pico-sdk-tools'
    Check   = 'pioasm\pioasm.exe'
  },
  @{
    # picotool 2.1.1（SDK 生成 .uf2 必需，版本必须 >=2.1.1）
    Name = 'picotool'
    Primary = 'https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.1.1-3/picotool-2.1.1-x64-win.zip'
    Mirror  = 'https://ghproxy.com/https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.1.1-3/picotool-2.1.1-x64-win.zip'
    TopDir  = 'picotool'    # 顶层是 picotool\ 目录
    Dest    = 'tools-2.1.1\picotool'
    Check   = 'picotool\picotool.exe'
  }
)

function Invoke-Download {
  param([string]$Url, [string]$Out)
  $ok = $false
  try {
    Write-Host "  GET $Url"
    $wr = @{ Uri = $Url; OutFile = $Out; TimeoutSec = 900 }
    if ($proxy) { $wr['Proxy'] = $proxy }
    Invoke-WebRequest @wr
    $ok = (Test-Path $Out) -and ((Get-Item $Out).Length -gt 1MB)
  } catch { Write-Warning "  失败: $_" }
  return $ok
}

foreach ($d in $deps) {
  $destPath = Join-Path $base $d.Dest
  $checkPath = Join-Path $destPath $d.Check
  if (Test-Path $checkPath) { Write-Host "[skip] $($d.Name) 已存在 -> $checkPath"; continue }

  $tmpZip = Join-Path $env:TEMP "$($d.Name).zip"
  $tmpExt = Join-Path $env:TEMP "$($d.Name)_extract"
  Remove-Item -Force -Recurse -ErrorAction SilentlyContinue $tmpZip, $tmpExt

  Write-Host "[下载] $($d.Name)"
  $got = Invoke-Download -Url $d.Primary -Out $tmpZip
  if (-not $got) { Write-Host '  直连失败，尝试国内镜像'; $got = Invoke-Download -Url $d.Mirror -Out $tmpZip }
  if (-not $got) { Write-Error "无法下载 $($d.Name)，请手动下载后放到 $tmpZip 再重跑本脚本。"; exit 1 }

  Write-Host "[解压] $($d.Name) -> $destPath"
  Expand-Archive -Path $tmpZip -DestinationPath $tmpExt -Force
  $src = if ($d.TopDir) { Join-Path $tmpExt $d.TopDir } else { $tmpExt }
  New-Item -ItemType Directory -Force -Path $destPath | Out-Null
  # 把 src 内的全部内容复制到 destPath（保留目录结构）
  Get-ChildItem -Path $src | ForEach-Object {
    $t = Join-Path $destPath $_.Name
    if ($_.PSIsContainer) { Copy-Item -Path $_.FullName -Destination $t -Recurse -Force }
    else { Copy-Item -Path $_.FullName -Destination $t -Force }
  }
  Remove-Item -Force -Recurse -ErrorAction SilentlyContinue $tmpZip, $tmpExt

  if (Test-Path $checkPath) { Write-Host "[ok] $($d.Name) 安装完成" } else { Write-Error "$($d.Name) 校验失败: $checkPath 不存在" }
}

Write-Host "`n全部依赖就绪，目录: $base"
Write-Host "构建时设置:  `$env:PICO_SDK_PATH = $base\sdk\2.1.1  ; 并把 $base\toolchain\14_2_Rel1\bin、$base\cmake\v3.31.5\bin、$base\ninja\v1.12.1 加入 PATH"
