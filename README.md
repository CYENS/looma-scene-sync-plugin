# Looma Scene Sync

Unreal Engine 5.6 runtime plugin providing bidirectional realtime scene sync between Unreal
and the LoomaXR web app — a WebSocket relay plus runtime GLB loading.

Part of the [LoomaXR](https://github.com/CYENS/hamlet-loomaxr) project.

## Contents

| Type | Class | Purpose |
| --- | --- | --- |
| Engine subsystem | `ULoomaSceneSyncSubsystem` | Owns the WebSocket connection, dispatches scene messages, loads GLBs at runtime |
| Actor | `ALoomaSyncedActor` | An actor whose transform/lifetime mirrors an object in the web scene |

Module `LoomaSceneSync` is `Runtime`, loading phase `Default`.

## Requirements

- Unreal Engine 5.6
- [glTFRuntime](https://github.com/CYENS/glTFRuntime) — declared in `LoomaSceneSync.uplugin` and
  linked as a public dependency in `LoomaSceneSync.Build.cs`. It must be present in the same
  project's `Plugins/` directory or the module will not compile.

Engine modules used: `Core`, `CoreUObject`, `Engine`, `glTFRuntime` (public); `WebSockets`, `Json` (private).

## Installation

Drop into your project's `Plugins/` directory as `LoomaSceneSync` — the directory name must match
the module name — then regenerate project files and rebuild.

In the LoomaXR plugin-development project this is consumed as a git submodule at
`Plugins/LoomaSceneSync`, alongside `Plugins/glTFRuntime`.

## Status

Beta (`IsBetaVersion: true`), version 0.1.0.
