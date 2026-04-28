param(
  [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"

Push-Location (Join-Path $PSScriptRoot "..")
try {
  $root = (Get-Location).Path
  $buildDir = Join-Path $root "build"
  $toolchain = Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"

  if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT 未设置，请先配置 vcpkg 环境变量。"
  }

  if (-not (Test-Path $toolchain)) {
    throw "未找到 vcpkg toolchain: $toolchain"
  }

  cmake -S . -B $buildDir -DCMAKE_BUILD_TYPE=$BuildType -DCMAKE_TOOLCHAIN_FILE="$toolchain"
  cmake --build $buildDir --config $BuildType

  $exe = Join-Path $buildDir "tomato_server.exe"
  if (-not (Test-Path $exe)) {
    $exe = Join-Path $buildDir "$BuildType/tomato_server.exe"
  }
  if (-not (Test-Path $exe)) {
    throw "构建成功但未找到 tomato_server.exe"
  }

  & $exe
}
finally {
  Pop-Location
}
