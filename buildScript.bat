@echo off

scons "platform=windows" "module_mono_enabled=yes" "arch=x86_64" "precision=double" "dev_build=yes" "use_llvm=yes" "use_mingw=yes" "debug_symbols=yes" "separate_debug_symbols=yes" "-j30"
if %errorlevel% neq 0 (
    echo "First scons build failed with error %errorlevel%"
    exit /b %errorlevel%
)
.\bin\godot.windows.editor.dev.double.x86_64.llvm.mono.console.exe "--verbose" "--generate-mono-glue" "modules\mono\glue" "--precision=double"
if %errorlevel% neq 0 (
    echo "Glue generation failed with error %errorlevel%"
    exit /b %errorlevel%
)
@REM ::python "%CD%\modules\mono\build_scripts\build_assemblies.py" "--godot-output-dir" "%CD%\bin" "--push-nupkgs-local" "%CD%\..\GodotNugetSourceData" "--precision=double"
python "%CD%\modules\mono\build_scripts\build_assemblies.py" "--godot-output-dir" "%CD%\bin" "--godot-platform=windows" "--push-nupkgs-local" "%CD%\..\GodotNugetSourceData" "--precision=double"
scons "platform=windows" "module_mono_enabled=yes" "arch=x86_64" "precision=double" "dev_build=yes" "use_llvm=yes" "use_mingw=yes" "separate_debug_symbols=yes" "-j30"
::COPY  "%CD%\..\GodotNugetSourceData" "%APPDATA%\NuGet\GodotNugetSource"
::COPY  "%CD%\..\GodotNugetSourceData" "%APPDATA%\NuGet\LocalNugetSource"

scons "target=template_release" "tools=no" "platform=windows" "module_mono_enabled=yes" "arch=x86_64" "precision=double" "dev_build=yes" "use_llvm=yes" "use_mingw=yes" "separate_debug_symbols=yes" "-j30"
scons "target=template_debug" "tools=no" "platform=windows" "module_mono_enabled=yes" "arch=x86_64" "precision=double" "dev_build=yes" "use_llvm=yes" "use_mingw=yes" "separate_debug_symbols=yes" "-j30"
