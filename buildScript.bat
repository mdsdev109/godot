@echo off
setlocal
cd /d "%~dp0"

REM Default: optimized double-precision editor used by planet-demo/run.bat.
REM buildScript.bat dev  : developer build with symbols (separate executable).
REM buildScript.bat glue : regenerate Mono glue/assemblies after an API change.
set "DEV=no"
set "SUFFIX="
if /I "%~1"=="dev" (
    set "DEV=yes"
    set "SUFFIX=.dev"
)

scons platform=windows d3d12=no module_mono_enabled=yes arch=x86_64 precision=double voxel_tests=yes use_llvm=yes use_mingw=yes dev_build=%DEV% debug_symbols=%DEV% separate_debug_symbols=%DEV% optimize=speed_trace -j24
if errorlevel 1 exit /b %errorlevel%

REM Ordinary field/shader changes do not require rewriting the Mono assemblies.
if /I not "%~1"=="glue" exit /b 0
"bin\godot.windows.editor%SUFFIX%.double.x86_64.llvm.mono.console.exe" --headless --generate-mono-glue modules\mono\glue --precision=double
if errorlevel 1 exit /b %errorlevel%
python modules\mono\build_scripts\build_assemblies.py --godot-output-dir bin --godot-platform=windows --precision=double
if errorlevel 1 exit /b %errorlevel%
scons platform=windows d3d12=no module_mono_enabled=yes arch=x86_64 precision=double voxel_tests=yes use_llvm=yes use_mingw=yes dev_build=no debug_symbols=no separate_debug_symbols=no optimize=speed_trace -j24
exit /b %errorlevel%
