# Builds the editor as a double-clickable Windows program in build-export\AceEngine\
# (AceEngine.exe + ace_data; D3D12, plus Vulkan when the Vulkan SDK is installed)
# and adds an "AceEngine" Start menu shortcut.
#   powershell -ExecutionPolicy Bypass -File tools\make_editor_windows.ps1 [output_dir]
# Needs Visual Studio (or its Build Tools) with "Desktop development with C++".
# Like the macOS app, File > Export still uses this source tree, so the result
# is meant for the machine it was built on.
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $Root "build-export" }
$Build = Join-Path $Root "build-export\editor-build"

# The compiler, cmake and ninja come from Visual Studio's developer environment.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  # An install with both the compiler and its bundled CMake + Ninja.
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
  if (-not $vs) { throw "Visual Studio with 'C++ CMake tools for Windows' not found" }
  & (Join-Path $vs "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = [Environment]::GetEnvironmentVariable("VULKAN_SDK", "Machine") }

cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DACE_EDITOR_WINDOWED=ON | Out-Null
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $Build --target ace_editor
if ($LASTEXITCODE) { throw "build failed" }

$App = Join-Path $Out "AceEngine"
if (Test-Path $App) { Remove-Item -Recurse -Force $App }
New-Item -ItemType Directory -Force $App | Out-Null
Copy-Item (Join-Path $Build "bin\ace_editor.exe") (Join-Path $App "AceEngine.exe")
Copy-Item -Recurse (Join-Path $Build "bin\ace_data") (Join-Path $App "ace_data") # shaders are found next to the exe

$Shortcut = Join-Path ([Environment]::GetFolderPath("Programs")) "AceEngine.lnk"
$link = (New-Object -ComObject WScript.Shell).CreateShortcut($Shortcut)
$link.TargetPath = Join-Path $App "AceEngine.exe"
$link.WorkingDirectory = $App
$link.Save()
Write-Host "built: $App\AceEngine.exe"
Write-Host "Start menu: $Shortcut"
