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
| `ULoomaSceneSyncSettings` | Project Settings > Plugins > Looma Scene Sync — the backend URL and the rendering knobs, read live |
| `ALoomaSyncedActor` | One scene **node**: a transform, with the engine components its wire components ask for |
| `LoomaSceneComponents` | The wire's `components` array parsed into keyed specs (`model` / `mesh` / `material` / `light`) |
| `LoomaWireConvert` | Wire↔UE transform conversion, shared by the scene-sync layer and the generation-job parser |
| `ELoomaJobState` / `LoomaGenerationTypes` | Text→3D job lifecycle, mirroring the backend's `JobState` vocabulary |
| `ULoomaGenerationHandle` | One generation job's events, scoped to that job — raw state events plus per-stage ones (queued / generating images / awaiting selection / generating asset / generated), and the calls to drive the job |
| `ULoomaSubmitGenerationAction` | Async Blueprint node — submit a text→3D job (`POST /generate`) |
| `ULoomaDownloadImageAction` | Async Blueprint node — download a candidate/selected image, decode to a transient `UTexture2D` |

Module `LoomaSceneSync` is `Runtime`, loading phase `Default`.

## The scene format

This plugin speaks **scene format v3**. The normative contract is
`docs/scene-format.md` in looma-xr-asset-demo; what follows is only how it lands in Unreal.

A scene is a **flat list of nodes with parent pointers** — `{id, parent, name, t, components?}`.
There is one kind of node, an object with a transform, and what it *is* comes from the components
attached to it. So one node is one `ALoomaSyncedActor`, attached to its parent's actor, whose root
is a plain `USceneComponent`; a node with no components is an empty object, and a perfectly
ordinary one, because its children's transforms are relative to it.

**`t` is parent-local.** World poses are composed by UE attachment, exactly as three.js composes
them in the browser. The axis/unit conversion is unchanged and still per node — a change of basis
composes across a parent chain.

**Structure edited in Unreal is reported back.** The same per-tick poll that diffs each actor's
pose also diffs its *attachment* against the parent the hub last named, so dragging a row onto
another in the World Outliner sends a `reparent` — carrying the node's new parent-local pose, and
ahead of any `transform` for it, so the object stays put in both clients. Attaching to an actor
that is **not** a synced node cannot be expressed (the wire addresses parents by node id and there
is none for a stray level actor): the node is reported as a root at its **world** pose, with one
warning naming the actor. Component edits are not reported outbound yet.

| Wire component | Unreal |
| --- | --- |
| `model` | `UStaticMeshComponent` built from `<BackendUrl>/static/<assetId>.glb` via glTFRuntime (the whole node tree merged into one mesh). The wire's `url` is ignored inbound — the path is rebuilt from `assetId` — but a spawn from Unreal **does** emit one (`WebAssetPrefix` + `/static/<assetId>.glb`), because a web peer renders nothing without it and the hub will not invent it. Lifted so its bounding-box floor sits on the node origin, matching the web client |
| `mesh` | `UStaticMeshComponent` with an `/Engine/BasicShapes` primitive — box / sphere / plane / cylinder. `size` is an **extent in metres**, so the primitive is measured and scaled to fit; pivot is the node origin (a box at y = 0 sits half in the floor, as in the browser) |
| `material` | A `UMaterialInstanceDynamic` on the sibling `mesh`, from `BasicShapeMaterial`: `color` (sRGB hex → linear) and `roughness`. Never applied to a `model`, which carries its own materials. `metalness` has no parameter to drive and is ignored |
| `light` | `UPointLightComponent` / `USpotLightComponent` / `UDirectionalLightComponent`. Aim convention: **the node's local −Y is the beam direction** |
| anything else | **Skipped, with the node kept** — logged once per type. That rule is what lets a new component type ship without a coordinated release across three clients |

### Light intensity

three.js in its physically-correct mode (which the web client uses, `decay = 2`) measures point
and spot lights in **candela** and directional lights in **lux**. UE measures a local light in
whichever `ELightUnits` it is told, and a directional light in lux. So the conversion is **1:1 in
matching units** — `IntensityUnits = Candelas` for point/spot, the bare number for directional —
with no magic constant.

What does *not* match is exposure: UE's auto-exposure and tone mapper are not three.js's, so the
same candela value can read brighter or darker on screen. `LightIntensityScale` (config, default
`1.0`) is the trim for that, and the only place a fudge factor belongs.

`distance: 0` means "no falloff limit" in three.js, which UE cannot express — a local light is
always clamped at its attenuation radius — so it maps to 10 m. `angle` is a half-angle in both
engines (radians vs degrees), and `penumbra` becomes the inner cone: `inner = outer × (1 −
penumbra)`.

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

Engine modules: `Core`, `CoreUObject`, `Engine`, `DeveloperSettings`, `glTFRuntime` (public);
`WebSockets`, `Json`, `HTTP`, `JsonUtilities`, `ImageWrapper` (private).

## Installation

Drop into your project's `Plugins/` directory as `LoomaSceneSync` — the directory name must match
the module name — then regenerate project files and rebuild.

## Configuration

**Edit > Project Settings > Plugins > Looma Scene Sync** (`ULoomaSceneSyncSettings`), saved to
`Config/DefaultGame.ini` under `[/Script/LoomaSceneSync.LoomaSceneSyncSettings]`:

| Setting | Default | What it does |
| --- | --- | --- |
| `BackendUrl` | `http://127.0.0.1:8000` | Where the backend is. Either a bare `host:port` (assumed http) or a full URL including a path prefix — `https://host/api` — for a reverse proxy or tunnel. The REST/asset base is this; the hub is this with a ws/wss scheme plus `/ws/scene` |
| `LightIntensityScale` | `1.0` | Trim for the exposure difference between UE and three.js — not a unit conversion, see above |
| `bBaseAlignModels` | `true` | Stand a `model`'s GLB on the node origin, as the web client does |
| `WebAssetPrefix` | `/api` | The proxy prefix a *web* peer needs in front of `/static/...`, used to fill in the `url` of a `model` we spawn |

Settings are read live from the CDO, so an edit needs no restart; changing `BackendUrl` reconnects
by itself, including mid-PIE. Locally, prefer an explicit IPv4 address over `localhost` — UE's
WebSocket client may resolve it to IPv6 (`::1`) while uvicorn listens on IPv4 only.

The knobs used to be `UPROPERTY(Config)` on the subsystem, so the old section
`[/Script/LoomaSceneSync.LoomaSceneSyncSubsystem]` is still read as a fallback for any key the
new one does not set (`BackendHost` feeding `BackendUrl`). The first save from the panel writes
the new section, which then wins.

### Console commands

| Command | What it does |
| --- | --- |
| `Looma.Reconnect` | Drops the socket and connects again immediately, re-reading the settings. `ULoomaSceneSyncSubsystem::Reconnect`, also Blueprint-callable |
| `Looma.Status` | Logs state (`CONNECTED` / `CONNECTING` / `DISCONNECTED (retry in Ns)` / `NO SOCKET`), the hub URL actually in use, the REST base, node/job counts and the active scene id — then `GET /health` and logs whether the backend answered, which separates "the socket is down" from "nothing is listening" |

Both work in the editor console, in PIE and in a packaged build; they resolve the subsystem from
the current game instance, and report the configured URL when nothing is running.

## Status

Beta (`IsBetaVersion: true`), version 0.1.0.
