# Godot fork (branch `planet`)

Instructions are in **[AGENTS.md](AGENTS.md)** beside this file, so Claude Code and Codex read
the same thing. Read it before changing anything here.

Fast orientation:

- **The game project is elsewhere:** `C:\Users\mschw\Home\Godot\Projects\planet-demo`. If the
  task is about the game rather than the engine, start from its `AGENTS.md`.
- Work on branch `planet`. `master` is upstream Godot.
- `modules/voxel` is a **separate repo** — commit voxel changes there.
- After any engine API change run `buildScript.bat glue`, or the editor segfaults on startup.
- Never benchmark a `buildScript.bat dev` binary; it is `-O0`.
