#!/bin/sh
set -e
cd "$(dirname "$0")"

# Linux twin of buildScript.bat -- keep the two in step.
# Default: optimized double-precision editor used by planet-demo.
# buildScript.sh dev  : developer build with symbols (separate executable).
# buildScript.sh glue : regenerate Mono glue/assemblies after an API change.
DEV=no
SUFFIX=
if [ "$1" = "dev" ]; then
    DEV=yes
    SUFFIX=.dev
fi

scons platform=linuxbsd use_llvm=yes module_mono_enabled=yes arch=x86_64 precision=double voxel_tests=yes dev_build=$DEV debug_symbols=$DEV separate_debug_symbols=$DEV optimize=speed_trace -j$(nproc)

# Ordinary field/shader changes do not require rewriting the Mono assemblies.
[ "$1" = "glue" ] || exit 0
"bin/godot.linuxbsd.editor$SUFFIX.double.x86_64.llvm.mono" --headless --generate-mono-glue modules/mono/glue --precision=double
# Publish Godot.NET.Sdk and friends to a local NuGet source, where game projects restore them from.
mkdir -p "$HOME/.MyLocalNugetSource"
dotnet nuget list source | grep -q MyLocalNugetSource || dotnet nuget add source "$HOME/.MyLocalNugetSource" --name MyLocalNugetSource
python3 modules/mono/build_scripts/build_assemblies.py --godot-output-dir bin --godot-platform=linuxbsd --precision=double --push-nupkgs-local "$HOME/.MyLocalNugetSource"
scons platform=linuxbsd use_llvm=yes module_mono_enabled=yes arch=x86_64 precision=double voxel_tests=yes dev_build=no debug_symbols=no separate_debug_symbols=no optimize=speed_trace -j$(nproc)
