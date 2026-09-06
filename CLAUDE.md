# Godot fork (branch `planet`)

@AGENTS.md

The file above is imported automatically. It covers the branch layout, what this fork adds,
how to build, and the traps that cost the most time here.

**The game project is elsewhere:** `C:\Users\mschw\Home\Godot\Projects\planet-demo`. If the task
is about the game rather than the engine, start from its `CLAUDE.md` — it auto-loads the
orientation and workflow docs that explain the whole project.

Before changing the planet field, read
`C:\Users\mschw\Home\Godot\Projects\planet-demo\docs\INVARIANTS.md`. The scalar, batched and
GLSL evaluations must stay bit-identical, and a new `PlanetFieldData` member does not reach the
collider unless you pack it, unpack it, and add it to `planet_field_equal()`.
