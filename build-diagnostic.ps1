$ErrorActionPreference = 'Stop'
$compiler = Join-Path $PSScriptRoot 'toolchain/llvm-mingw-20260616-ucrt-x86_64/bin/clang++.exe'
$output = Join-Path $PSScriptRoot 'build-windows-diagnostic'
New-Item -ItemType Directory -Force $output | Out-Null
& $compiler --target=x86_64-w64-mingw32 -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wno-cast-function-type -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -shared -static (Join-Path $PSScriptRoot 'src/dinput8_proxy.cpp') (Join-Path $PSScriptRoot 'src/mod.cpp') (Join-Path $PSScriptRoot 'src/dinput8.def') -o (Join-Path $output 'dinput8.dll') -ld3d11 -ldxgi -lgdi32 -luser32
if ($LASTEXITCODE -ne 0) { throw 'DLL compilation failed' }
& $compiler --target=x86_64-w64-mingw32 -std=c++20 -O2 -static (Join-Path $PSScriptRoot 'tests/proxy_smoke.cpp') -o (Join-Path $output 'proxy_smoke.exe') -ldxguid
if ($LASTEXITCODE -ne 0) { throw 'Smoke test compilation failed' }
