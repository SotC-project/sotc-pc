@echo off
rem Configure (first run) and build the SotC runtime. Usage: build.bat [target]
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%~dp0toolchain\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin;%PATH%"
cd /d "%~dp0runtime"
rem HAVE_ENDIAN_H=0: clang 23 ships an endian.h that libressl's Windows shim mis-detects.
if not exist Build\x64-Clang-RelWithDebInfo\configured.ok (
  cmake --preset x64-Clang-RelWithDebInfo -DHAVE_ENDIAN_H=0 || exit /b 1
  echo ok> Build\x64-Clang-RelWithDebInfo\configured.ok
)
cmake --build Build\x64-Clang-RelWithDebInfo %*
