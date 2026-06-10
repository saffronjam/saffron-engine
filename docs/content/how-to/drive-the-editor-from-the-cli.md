+++
title = 'Drive the editor from the CLI'
weight = 5
math = false
+++

# Drive the editor from the CLI

Script and inspect a running editor with the `se` CLI over its unix socket.

## Steps

1. Start the editor; the control socket comes up with it:
   ```sh
   se start          # or: se start --build, or --attach to keep it foreground
   ```
   `se start` launches `SaffronEngine` in the toolbox. Every other command talks JSON over the socket.
2. Check liveness and list commands:
   ```sh
   se ping
   se help
   ```
3. Drive the scene. Each command is `se <command> [positional...] [--flag value]`:
   ```sh
   se list-entities
   se select MyEntity
   se set-transform MyEntity --translation '{"x":0,"y":1,"z":0}'
   se pick --u 0.5 --v 0.5          # ray-pick at viewport UV (0,0 = top-left)
   se focus MyEntity                # aim the editor camera at it
   ```
4. Add `-o json` to any command for raw JSON to pipe to `jq`:
   ```sh
   se inspect MyEntity -o json | jq .components
   ```

## Verify

- Read the frame's draw counters: `se render-stats` reports `drawCalls` / `batches` / `instances`, frame timing (`frameMs` / `fps`; `gpuMs` is 0 until a GPU timestamp readback exists), plus feature flags.
- Capture what's on screen:
  ```sh
  se screenshot viewport /tmp/view.png    # the offscreen scene image
  se screenshot window   /tmp/full.png    # the whole window (written end-of-frame)
  ```
- Editor panels update live as commands land.

## Driving animation

A rig imported from a glTF carries an animation player and its clips. Play one straight from the
shell — it previews live in Edit, no need to enter play mode:

```sh
se list-clips <rig>                       # {id, name, duration} for each clip
se play-animation <rig> Walk --loop       # previews in Edit; --blend 0.2 to cross-fade in
se get-animation-state <rig>              # watch `time` advance, `playing:true`
se seek-animation <rig> 0.5               # scrub the playhead (pauses-and-shows the pose)
se set-animation-loop <rig> pingpong      # once | loop | pingpong
se stop-preview <rig>                     # revert to the rest pose
```

`<rig>` is the skinned mesh entity (an id or its name); a clip is an id or its catalog name.

## In the code

| What | File | Symbols |
|---|---|---|
| `start` wrapper + socket path | `cmd/se` | `cmd_start`, `socket_path` |
| CLI request/reply + arg coercion | `tools/se/source/main.cpp` | `buildParams`, `coerce`, `printResult` |
| Scene commands | `control_commands_scene.cpp` | `select`, `set-transform`, `pick`, `focus`, `inspect` |
| Render stats + screenshot | `control_commands_render.cpp`, `control_commands_asset.cpp` | `render-stats`, `screenshot` |

## Related

- [Control plane](../../explanations/tooling-and-control/control-plane-architecture/)
- [Picking](../../explanations/scene-and-ecs/picking/)
