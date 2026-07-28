# Looma Scene Sync

Unreal Engine 5.6 runtime plugin providing bidirectional realtime scene sync between Unreal
and the LoomaXR web app — a WebSocket relay, runtime GLB loading, and text→3D generation jobs.

Part of the [LoomaXR](https://github.com/CYENS/hamlet-loomaxr) project. The canonical source of
this plugin lives in [looma-xr-asset-demo](https://github.com/CYENS/looma-xr-asset-demo) at
`unreal/LoomaSceneSync`, which consumes this repo as a submodule.

## Contents

| Unit | Purpose |
| --- | --- |
| `ULoomaSceneSyncSubsystem` | Owns the WebSocket connection, dispatches scene messages, loads GLBs at runtime, spawns synced assets |
| `ALoomaSyncedActor` | An actor whose transform/lifetime mirrors an object in the web scene |
| `LoomaWireConvert` | Wire↔UE transform conversion, shared by the scene-sync layer and the generation-job parser |
| `ELoomaJobState` / `LoomaGenerationTypes` | Text→3D job lifecycle, mirroring the backend's `JobState` vocabulary |
| `ULoomaGenerationHandle` | One generation job's events, scoped to that job |
| `ULoomaSubmitGenerationAction` | Async Blueprint node — submit a text→3D job (`POST /generate`) |
| `ULoomaDownloadImageAction` | Async Blueprint node — download a candidate/selected image, decode to a transient `UTexture2D` |

Module `LoomaSceneSync` is `Runtime`, loading phase `Default`.

### Coordinate conventions

Wire format is right-handed, Y-up, metres, quaternion `[x,y,z,w]`; Unreal is left-handed, Z-up,
centimetres. The mapping must match glTFRuntime's **default** `SceneBasis`
(`FglTFRuntimeConfig::GetMatrix`), which maps `(x, y, z) -> (-z, x, y)`, so mesh geometry and
actor transforms agree. That map flips handedness (det = -1), so rotations conjugate as
`q_UE = (qz, -qx, -qy, qw)`. See `LoomaWireConvert.h`.

## Requirements

- Unreal Engine 5.6
- [glTFRuntime](https://github.com/CYENS/glTFRuntimeLoomaXR) — declared in `LoomaSceneSync.uplugin`
  and linked as a public dependency in `LoomaSceneSync.Build.cs`. It must be present in the same
  project's `Plugins/` directory or the module will not compile.

Engine modules: `Core`, `CoreUObject`, `Engine`, `glTFRuntime` (public); `WebSockets`, `Json`,
`HTTP`, `JsonUtilities`, `ImageWrapper` (private).

## Installation

Drop into your project's `Plugins/` directory as `LoomaSceneSync` — the directory name must match
the module name — then regenerate project files and rebuild.

`BackendHost` in `LoomaSceneSyncSubsystem.h` defaults to `127.0.0.1:8000`; point it at your backend
(or a tunnel host) as needed.

## Status

Beta (`IsBetaVersion: true`), version 0.1.0.
