# Godot fork — agent entry point

This is a fork of `godotengine/godot` carrying the engine side of a planetary game.

**You are probably here for the game, not the engine.** The project is at
`C:\Users\mschw\Home\Godot\Projects\planet-demo`, and its `AGENTS.md` and `docs/ORIENTATION.md`
are the place to start. Come back here only for engine-level changes.

## Branches

| Branch | What |
|---|---|
| `master` | upstream Godot. Leave it alone. |
| **`planet`** | **the work.** Everything below assumes you are on it. |

`modules/voxel` is a **separate repository** (fork of `Zylann/godot_voxel`, also on a `planet`
branch) and is gitignored here. Voxel changes commit there, not here.

There used to be a `planet_work/` scratch and staging directory at this root. It is gone:
work happens in git worktrees of the game project now, and nothing should recreate it. See
`C:\Users\mschw\Home\Godot\Projects\planet-demo\docs\WORKFLOW.md`.

Do not push without the user asking. Remotes: `github.com/mdsdev109/godot` and
`github.com/mdsdev109/godot_voxel`, branch `planet` on both.

## What this fork adds

An Earth-sized planet cannot be a mesh — at 6,371,000 m a float32 ULP is 0.25 m. So the
surface is a pure function and the collider samples it directly, with no geometry in between.

```
core/math/planet_field.h         f, the definition of the surface. Header-only.
core/math/planet_biome.*         biome table, blending, macro map registry
core/math/planet_coast.h         coastlines and shoreline geology
core/math/planet_lattice.h       the cube-sphere lattice
core/math/planet_hash_noise.h    the noise
core/math/planet_hash_noise_shader.h   its exact GLSL twin -- EDIT WITH THE FILE ABOVE

scene/resources/3d/planet_{field,biome,shape}_3d.*   script-facing resources
scene/resources/3d/planet_cliff_skin_3d.*    the rock face over steep walls (planet-demo's cliff_skin.gd
                                             tunes it); "cuts" keep corridors bare where a road climbs
core/math/planet_det_math.h      sin/atan2/pow/... that give the same bits on every machine
servers/physics_3d/              SHAPE_PLANET, plumbed through the server, dummy and wrap_mt
modules/jolt_physics/shapes/     JoltPlanetShape3D and JoltCustomPlanetShape
modules/planet_net/              the client/server layer: PlanetNetServer, PlanetNetClient over ENet
```

The field must stay free of C runtime transcendentals -- use `planet_det_math.h` -- or Windows and
Linux compute different terrain. The same goes for game scripts that place anything the network
server must place too (rocks, cliffs, the lattice mapping): GDScript's `sin`/`cos`/`tan` and
`Basis(axis, angle)` are the C runtime; `PlanetMath` (modules/planet_net/planet_math.h) is
`PlanetDetMath` for scripts. See planet-demo's `docs/DETERMINISM.md`. The network layer's design
is planet-demo's `docs/NETWORKING.md`.

## Building

```
buildScript.bat          # optimized double-precision Mono editor -- what the project runs
buildScript.bat dev      # separate dev executable, symbols, -O0
buildScript.bat glue     # regenerate Mono glue + assemblies. NOT OPTIONAL after an API change.
```

On Linux (what the script runs, minus the Windows flags; not yet tried on this fork):

```
scons platform=linuxbsd module_mono_enabled=yes precision=double voxel_tests=yes optimize=speed_trace -j$(nproc)
bin/godot.linuxbsd.editor.double.x86_64.mono --headless --generate-mono-glue modules/mono/glue --precision=double
python modules/mono/build_scripts/build_assemblies.py --godot-output-dir bin --godot-platform=linuxbsd --precision=double
```

`modules/voxel` is a separate clone: `git clone -b planet https://github.com/mdsdev109/godot_voxel.git modules/voxel`.

- **`precision=double` is mandatory.** Single precision cannot represent this radius.
- **After any API or precision change, run `buildScript.bat glue`.** Otherwise the editor
  segfaults on startup on an empty project with nothing printed past the Vulkan banner,
  because C# assemblies are precision-specific and the mismatch is an ABI break. Running with
  `--path` still works, which makes it look like a project bug.
- **Never benchmark on a `dev` build.** `dev_build=yes` implies `optimize=none`; measurements
  taken on one have already produced a completely inverted conclusion.
- **Engine work is serialized.** There is one `bin/` and builds are long — do not start engine
  work while another agent is building. Say so before you begin.

## Tests

Both need `voxel_tests=yes` (the default in `buildScript.bat`) and live in `modules/voxel`:

```
bin\godot.windows.editor.double.x86_64.llvm.mono.console.exe --headless --quit --run_planet_field_check
bin\godot.windows.editor.double.x86_64.llvm.mono.console.exe --headless --quit --run_planet_field_bench
```

`--quit` is required: they run at module init, and without it the engine boots a headless
project manager afterwards and never exits.

The check compares scalar, batched and voxel-graph evaluation **bit for bit**. Approximate
agreement is not sufficient and it has caught two real defects that would have put a step at
every chunk boundary.

## Before you change the field

Read `C:\Users\mschw\Home\Godot\Projects\planet-demo\docs\INVARIANTS.md`. In particular:

- The scalar, batched and GLSL evaluations must stay bit-identical.
- A new `PlanetFieldData` member does **not** reach the collider unless you also pack it in
  `PlanetShape3D::_update_shape()`, unpack it in `JoltPlanetShape3D::set_data()`, and add it to
  `planet_field_equal()`. Missing the last one means edits never take effect at all.
- The noise's force-inlining, its hand-rolled `floor_fast`, and its use of raw pointers instead
  of `Span` indexing are each load-bearing for performance and easy to undo by accident.
