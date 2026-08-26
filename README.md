# Looma Scene Sync

Unreal Engine 5.6 runtime plugin providing bidirectional realtime scene sync between Unreal
and the LoomaXR web app — a WebSocket relay, runtime GLB loading, and text→3D generation jobs.

Part of the [LoomaXR](https://github.com/CYENS/hamlet-loomaxr) project. This repository is the
plugin's only home. It is developed in the UE 5.8 viewer's checkout of it,
`code/looma-xr-viewer-ue58/Plugins/LoomaSceneSync` in the umbrella repo — that is the project
it is built and tested against. The UE 5.6 viewer (`code/looma-xr-viewer`) consumes it as a
second submodule, so a change here reaches a consumer only once its gitlink is bumped.

The protocol it implements is specified in
[looma-xr-asset-demo](https://github.com/CYENS/looma-xr-asset-demo): `docs/scene-format.md`
(normative) and `docs/unreal-sync.md` (UE design notes).

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
| `FLoomaIdentity` / `ELoomaIdentityKind` | Who the backend says we are — its `IdentityOut`: user id, display name, kind (`Guest` / `User`), admin flag |
| `ULoomaLoginAction` | Async Blueprint node — log in (`POST /auth/login`) |
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
| `Looma.Status` | Logs state (`CONNECTED` / `CONNECTING` / `DISCONNECTED (retry in Ns)` / `NO SOCKET`), the hub URL actually in use, the REST base, the backend's auth state, node/job counts and the active scene id — then `GET /health` and logs whether the backend answered, which separates "the socket is down" from "nothing is listening". The `/health` answer is also what refreshes the auth state, so this doubles as "ask the backend again" |
| `Looma.Login <username> <password>` | Logs in over `POST /auth/login`. **Testing only** — the console echoes the password into the log and keeps it in the command history before the plugin sees a character of it. The real login is the `Login` Blueprint node behind a field that does not echo |
| `Looma.Logout` | Forgets the token, then asks the backend to revoke it |
| `Looma.Whoami` | Logs the current identity, and re-asks `GET /auth/me` when a token is held |

They work in the editor console, in PIE and in a packaged build; they resolve the subsystem from
the current game instance. `Looma.Reconnect` / `Looma.Status` report the configured URL when
nothing is running; the auth commands have no session to report without a subsystem, and say so.

### Auth discovery

The backend says whether it wants a login in the `auth` block of `GET /health` —
`{"enabled": bool, "registrationEnabled": bool}`, the discovery block HAM-172 added, and the same
one the web frontend reads to decide whether to draw a login screen. The plugin probes `/health`
on startup and whenever `BackendUrl` moves, so the answer is available before anything asks for
it, and re-probes on a lengthening backoff (5 s doubling to 60 s) for as long as the answer is
still `Unknown`. `Looma.Status` probes too, and is the only caller that logs the reachability
triage or gets a short timeout — a background probe is given 30 s, and never loops on a human's
behalf.

`Unknown` is a **transient** state, not a resting place, and the retry is what makes that true.
The reason is worth knowing, because the failure looks like a broken backend and is not one: UE
caps concurrent connections per server at 16 (`HttpMaxConnectionsPerServer`), and the GLB
downloads and the REST API share a host and therefore share that pool. On a populated scene over
a real network, `/health` queues behind up to 16 GLB fetches and can time out against a backend
answering in a tenth of a second — measured over a Cloudflare tunnel: `curl` 0.12-0.27 s, the
plugin's own request timing out at 5 s, with exactly 16 meshes building in the log. There is
deliberately no probe at socket-connect time for the same reason: that is the instant every node
starts pulling its GLB, which is the worst possible moment to want a connection slot.

`ULoomaSceneSyncSubsystem` exposes it as **three states, not a bool** (`ELoomaAuthState`), because
"this backend needs no login" and "we have not managed to ask yet" have to be told apart — a
client that skips the login screen because the probe has not landed is a bug, not a fast path.
All of it is Blueprint-visible, under the `Looma|Auth` category:

| Blueprint node | Meaning |
| --- | --- |
| `Get Auth State` | `Unknown` / `Disabled` / `Enabled Registration Closed` / `Enabled Registration Open` |
| `Is Auth State Known` | False while `Unknown`. Gate any "show the login screen" decision on this first |
| `Is Auth Enabled` | The backend requires a login. False while `Unknown` |
| `Is Registration Enabled` | The backend accepts self-registration. Only ever true when auth is enabled |
| `On Auth State Changed` | Fires when the state becomes known or moves, so a UI can react instead of polling |

Registration is folded into the enabled states rather than carried as a second flag, since
`registrationEnabled` only means anything when auth is on. An unreachable backend, a non-JSON
answer, and a `/health` with no `auth` block at all (a backend predating HAM-172) all leave the
state `Unknown` — never `Disabled`. Reading silence as "no auth" happens to be right against
today's backend, which is exactly why the code does not write it down.

### Logging in

`ELoomaAuthState` answers *what does this backend require*; the session API answers *who am I*.
The two move independently — a backend can demand a login while this client is still a guest,
which is the pair of facts a login screen exists to resolve — so they are separate state with
separate events (`On Auth State Changed` versus `On Identity Changed`).

| Blueprint node | Meaning |
| --- | --- |
| `Login` (async) | `POST /auth/login`. `On Logged In` carries the `FLoomaIdentity`, with the session already adopted; `On Failed` carries a message meant to be shown |
| `Logout` | Forgets the token and the identity, then asks the backend to revoke — in that order |
| `Refresh Identity` | `GET /auth/me`, adopting the answer |
| `Get Identity` | The `FLoomaIdentity` we last learned. `Kind == Unknown` until something establishes it |
| `Has Auth Token` | We hold a session token. Not the same question as "am I a user" — a token can be revoked server-side while we still hold the bytes |
| `Get Identity Text` | One line naming the identity and whether a token is held. Never the token |
| `On Identity Changed` | Who we are moved: a login, a logout, a refresh |

The token lives **in memory only**, for the lifetime of the game instance — no config, no save
game, no log line — so a fresh editor session is a fresh login. `ApplyAuthHeader` is the only
thing that reads it; it takes an HTTP request and sets the header, rather than returning the
string, so no caller ever holds a copy. Changing `BackendUrl` drops the session as well as the
auth state: a token minted by one backend means nothing at another, and sending it to whoever
now answers at that address would leak it for no benefit.

Failures are deliberately narrow. A rejected login changes nothing else — the socket stays up,
the scene stays loaded, and any identity already held is still held. A **401** never says whether
the username or the password was wrong, because the backend does not either: it verifies against
a dummy hash when no account exists, so the same message comes back after the same amount of work
(`backend/app/auth/local.py`), and second-guessing that here would rebuild the account
enumeration oracle it exists to deny. A **403** is a different thing worth its own message —
accounts are switched off server-side, so retyping cannot help.

Two subtleties of the backend contract that the code leans on:

- **`GET /auth/me` never answers 401.** An unauthenticated caller gets a *guest* identity with a
  200, so validity is decided by reading `kind`, never the status code. A token that comes back as
  a guest has been revoked or has expired, and is dropped.
- **A guest `/auth/me` answer is not adoptable.** Its HTTP path mints a fresh random
  `Guest-xxxxxx` per call when there is no session, so the name matches nothing any other client
  sees in the room roster. `Refresh Identity` therefore adopts an answer only when it is a user,
  and `Looma.Whoami` only re-asks when a token is held.

### Carrying the session

Every REST call the plugin makes goes out with `Authorization: Bearer <token>` when a session is
held — `SendRest`, the queue hydrate, `POST /generate`, the candidate-image download and the auth
routes themselves. `ApplyAuthHeader` is the only thing that attaches it, and it takes the request
rather than returning the string, so no caller ever holds a copy of the token. Because it is the
only attacher, it can also enforce that the token goes **nowhere but the configured backend**: it
compares the request URL against `BackendUrl` and withholds the header otherwise. That is not
theoretical — the image URL comes off a generation job and through `ResolveBackendUrl`, which
forwards an absolute URL untouched, so it is the one request whose host the plugin did not choose.
Call `SetURL` before `ApplyAuthHeader`; the wrong order costs you the header, which is the safe
failure.

**`/static/*.glb` is deliberately anonymous.** It is a plain `StaticFiles` mount and the FastAPI
app carries no app-wide `dependencies=` and no auth middleware — only individual routes have
`require_admin` — so a bearer there would be a token sent somewhere nothing reads it. glTFRuntime
*does* accept a header map (`glTFLoadAssetFromUrl`'s second parameter), so the seam exists if
`/static` is ever gated; the empty map in `LoadMeshFromUrl` is a decision, not an omission.

The WebSocket handshake carries the session two ways: as an `Authorization` upgrade header, and as
the `hello` message's `token` field. The hub's precedence is header, then session cookie, then
`hello.token` (`docs/scene-format.md`), and all three resolve to the same identity — the header
wins when it survives the trip, and the hello field covers a proxy that strips `Authorization` on
upgrade, which a tunnelled deployment makes a real case rather than a hypothetical one. One
consequence to respect when adding logging: the outbound `hello` now contains a credential, so
`SendJson` must never log what it serialises.

**A login re-opens the socket.** The hub resolves an identity once, from the handshake, and there
is no message that re-identifies a socket already up — so adopting or dropping a session
reconnects, and that is what makes a login visible to the other clients in the room instead of
only to this one. It is cheap: the reconnect re-sends the whole `scene`, but a node still in it
keeps the actor it already has, so `ApplyModel`'s `Context.ModelUrl != LoadedModelUrl` guard holds
and no GLB is re-fetched. Dropping a session only reconnects if one was actually held, so
`Looma.Logout` as a guest does not churn the room.

The round trip to test: `Looma.Login <user> <pass>` and the **web client's room roster** should
show the account's display name with `kind: "user"` in place of `Guest-xxxxxx`, and
`Looma.Logout` should put it back to a guest.

## Status

Beta (`IsBetaVersion: true`), version 0.1.0.
