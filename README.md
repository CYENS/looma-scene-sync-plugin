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
| `FLoomaClient` / `ELoomaClientKind` | One client in the room, from the hub's `clients` roster: id, colour, role, display name, kind (`Guest` / `User`), selection |
| `FLoomaBorderGroup` | What to draw for one remote client — its colour, its stencil slot, the nodes it won outright and their descendants |
| `ULoomaLoginAction` | Async Blueprint node — log in (`POST /auth/login`) |
| `ULoomaSubmitGenerationAction` | Async Blueprint node — submit a text→3D job (`POST /generate`) |
| `ULoomaDownloadImageAction` | Async Blueprint node — download a candidate/selected image, decode to a transient `UTexture2D` |
| `ULoomaLoginUI` | *(UI module)* Two Blueprint calls that put the bare-bones login form on the viewport, or take it off |

Two `Runtime` modules, both loading phase `Default`. `LoomaSceneSync` is the protocol and
runtime half and has no UI dependency at all; `LoomaSceneSyncUI` is optional and carries the Slate
one. Dependencies flow one way — the UI module depends on the core, never the reverse — so
removing the `LoomaSceneSyncUI` entry from `LoomaSceneSync.uplugin` leaves the core building and
behaving unchanged. That matters for a headless or dedicated-server build, which has no business
linking Slate.

To drop the UI module, **delete its entry** from the `Modules` array, or give it a
`TargetDenyList` / `PlatformDenyList`. Adding `"Enabled": false` to the module descriptor does
**nothing** — that key belongs to entries in the `Plugins` array, and UnrealBuildTool ignores
unrecognised keys on a module descriptor, so the module still compiles and links. Verified by
building it both ways.

### The login form

`LoomaSceneSyncUI` ships a **bare-bones login form** — a working starting point, not a designed
screen. Two Blueprint nodes, both under `Looma|Auth`:

| Node | What it does |
| --- | --- |
| **Show Login UI** | Adds the form to the viewport. Idempotent, so calling it from `BeginPlay` on a level that reloads does not stack copies |
| **Hide Login UI** | Takes it off. Safe when it is not showing |
| **Is Login UI Showing** | Whether it is up |

**Call `Show Login UI` unconditionally** — at startup, without first working out whether this
backend wants a login. The form decides for itself and collapses to nothing while the auth state
is unknown *and* while auth is disabled. That check is the one thing here most worth not
reimplementing: "we have not managed to ask yet" must not render as "no login needed", and because
the auth probe retries with backoff, that window can be seconds on a scene busy enough to saturate
the HTTP connection pool.

What it renders, given `LOOMA_AUTH_ENABLED=1` on the backend: username, a masked password, a
submit button that disables itself while a login is in flight, and an error line carrying the
backend's own wording — which is deliberately one generic message for a wrong password and an
unknown account alike. Once signed in, the name and a log-out button. A session restored from disk
but not yet re-validated says so, rather than passing itself off as confirmed.

The password is never stored on the widget and never logged. It is read from the box at submit and
handed straight to `RequestLogin`.

**Layout and styling live in C++**, because the plugin sets `"CanContainContent": false` and this
module keeps it that way — no `WBP`, nothing to cook, nothing to re-parent. If you want a designed
login screen, build your own widget against the `Looma|Auth` surface on
`ULoomaSceneSyncSubsystem` (`IsAuthEnabled`, `GetIdentity`, `Login`, `Logout`,
`OnAuthStateChanged`, `OnIdentityChanged`) rather than inheriting this one. An asset invites
editing; an API invites replacing.

One known limitation: the form is tracked in a single process-wide slot, so under *Play As Client*
with two PIE windows the second `Show Login UI` is a no-op. Both windows share one game-instance
subsystem and therefore one session, so a second form would be showing the first one's login
anyway.

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
| `GuestDisplayName` | *(empty)* | The name to suggest for this client in the room roster while connected as a guest. Empty suggests nothing and the hub falls back to `Guest-xxxxxx`. Ignored while logged in — see *Being named in the room* |
| `RemoteSelectionCollection` | *(empty)* | The Material Parameter Collection remote clients' border colours are written into. **It has to live in your project** — the plugin ships no assets — so this is a soft path. Empty publishes no colours; the stencil is still written. See *Wiring the outline* |

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
| `Looma.Status` | Logs state (`CONNECTED` / `CONNECTING` / `DISCONNECTED (retry in Ns)` / `NO SOCKET` / `REFUSED (code), not retrying`), the hub URL actually in use, the REST base, the backend's auth state, node/job counts, the performance id and the active scene id — then `GET /health` and logs whether the backend answered, which separates "the socket is down" from "nothing is listening". The `/health` answer is also what refreshes the auth state, so this doubles as "ask the backend again" |
| `Looma.Login <username> <password>` | Logs in over `POST /auth/login`. **Testing only** — the console echoes the password into the log and keeps it in the command history before the plugin sees a character of it. The real login is the `Login` Blueprint node behind a field that does not echo |
| `Looma.Logout` | Forgets the token, then asks the backend to revoke it |
| `Looma.Whoami` | Logs the current identity, and re-asks `GET /auth/me` when a token is held |
| `Looma.Performance [id [cue]]` | With an id, asks the hub for that performance and **reconnects** — a socket's performance is fixed at `hello` and never changes, so a switch is a reconnect and there is no message for it. The id is a request, never a grant: the hub re-runs the same non-enumerating gate as `GET /performances/{id}` and refuses with a bare `4403`/`4404` close, which stops the retry loop rather than being waited out. The request outlives the socket, so a later `Looma.Reconnect` keeps the room you chose — including back into a refusal, which the refusal line says it will. With no arguments, logs the performance this socket is in — id, name and visibility, all off the `scene` frame, so it costs nothing — and names any switch still in flight first, because until the new socket's frame lands the "current" performance is the one being left. A performance with no name is one whose row has been deleted; the id is still where the socket is. **With an id and a cue index** it does both: the cue is parked and opened once the new socket is confirmed in a performance, because the socket that must carry the `openScene` does not exist yet and a send before it opens is dropped without trace. The rail is read for where we actually LAND, not for what was asked — so if the hub places this client elsewhere the cue is dropped rather than applied to a running order the user never saw, and said so. A syntactically bad or negative index is refused before the socket is touched; an out-of-range one can only be discovered after the reconnect, and the message says the switch itself stands |
| `Looma.Scene [sceneId \| "name"]` | With an argument, opens that saved scene **for this client only** — nobody else in the room moves, because switching scene is a message where switching performance is a reconnect. A refused id comes back as a `sceneError` frame, which is logged; the socket stays up and usable. Typed while the socket is down it says so, since an `openScene` send would otherwise be dropped silently and never retried. The argument may be the id or the **name**: it is resolved against `GET /scenes` first — exact id, then exact name, then name ignoring case — so every open costs one metadata request, a name matching two scenes is refused with the ids that tell them apart, and a miss is answered with the list of scenes this identity is offered instead of a bare `sceneError`, pointing at `Looma.Scenes` for the full one. An unquoted multi-word name is rejoined, so `Looma.Scene Costume Test` works and `Looma.Scene "Costume Test"` is the exact form. If the list cannot be read at all, the argument is sent as an id and the hub answers, which is what this command did before. With no arguments, logs the active scene's id at once and its name on a second line when `GET /scenes` answers — the `scene` frame carries no name, so the name costs a round trip, and it is fetched on demand rather than on scene arrival, which is the worst moment to want a connection slot |
| `Looma.Scenes` | Lists every scene this identity may open — id, name, and a `*` on the active one — one per line and **untruncated**, because this is the command whose whole purpose is the list. Reads `GET /scenes` and never touches the socket, so it answers while disconnected, which is when you are most likely working out where to reconnect to. Nothing marked means no saved scene is open; `Looma.Scene` with no arguments says which of the two reasons applies |
| `Looma.Performances` | Lists every performance this identity may see — id, name, and **our own role** in it (`editor` / `viewer`), which is what predicts whether a later edit is refused — marking where this socket is. During a switch the confirmed row is marked `(leaving)` and the requested one `(requested, not confirmed)`, since until the new socket's `scene` frame lands the “current” performance is the one being left. A row absent from this list is one this identity cannot join either, so the listing is an honest preview of what `Looma.Performance <id>` will accept |
| `Looma.Cue [index]` | With an index, opens the scene at that position on the current performance's running order — **0-based**, matching the backend's own numbering. It is an `openScene` and not a reconnect: a cue names a scene in the performance already connected. The index is a **position in the returned list**, not an `order_idx` value, because that column is neither guaranteed dense nor zero-based. Out of range reports the valid range rather than clamping, and it is refused outright while a performance switch is in flight, since the rail we would read belongs to the room being left. With no arguments, prints the running order — index, scene id, label — marking every cue whose scene is active: a scene can be on the line twice (a reprise) and the wire moves by scene rather than by position, so which of them is live is genuinely not knowable here. An empty running order is not a fault — the pool and the rail are different lists |
| `Looma.Select <nodeId...>` | Replaces the local selection and reports it to the room. Not additive — the wire carries a whole set, so this does too. Unknown ids are named and skipped |
| `Looma.Deselect [nodeId...]` | Deselects those nodes; with no arguments clears the whole selection, which is what removes this client's borders elsewhere |
| `Looma.Selection` | Logs the local selection — the ids the next `selection` message would carry |
| `Looma.Room` | Logs who else is in the room from the hub's roster — id, name, kind, role, colour and selection — with this client's own entry marked `*`. The only way to read our own room name, since `GET /auth/me` mints a fresh guest name per call |
| `Looma.Claims` | Logs the claim ledger both ways round: each claimed node with its claimants oldest-first (the head draws the border), then each client with how much of its selection it actually wins, then which stencil slots are in use and who the budget left out |

Between them these are self-sufficient: every id and name `Looma.Scene`, `Looma.Performance` and `Looma.Cue` accept can be discovered with `Looma.Scenes`, `Looma.Performances` and `Looma.Cue`, so nothing here needs a browser tab open beside it. The distinction that shapes all of them is that changing **scene** is a message while changing **performance** is a reconnect — a socket's performance is fixed at `hello` for its life.

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
| `Is Identity Provisional` | A session was restored from disk and `/auth/me` has not confirmed it yet. See *Staying logged in* |
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
only attacher, it can also enforce that the token goes **nowhere but the configured backend**: the
request URL must begin with `BackendUrl` *and* break on a path boundary — the next character has
to be `/`, or the URL has to be the base exactly. A bare prefix match would not do. Against the
default `http://127.0.0.1:8000`, `http://127.0.0.1:8000@evil.com/whatever` starts with the base
and is not the backend at all, because everything before the `@` is userinfo and the host is
`evil.com`; `…:8000.evil.com` and `…:80001` slip through the same hole. This is not theoretical —
the image URL comes off a generation job and through `ResolveBackendUrl`, which forwards an
absolute URL untouched, so it is the one request whose host the plugin did not choose. Call
`SetURL` before `ApplyAuthHeader`; the wrong order costs you the header, which is the safe
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

### Staying logged in

The session survives an editor restart, because the backend's own session TTL is 30 days and
retyping a password through a VR keyboard is the thing this exists to avoid. It is kept at:

```
<Project>/Saved/LoomaSceneSync/Session.json
{"backend":"http://127.0.0.1:8000","token":"…","displayName":"alice"}
```

**The token is plaintext on disk. There is no encryption and no DPAPI.** That is the same
exposure as the browser's session cookie, so it is not a regression against the web client, but
it is worth knowing rather than discovering: anyone who can read the project's `Saved/` directory
can use the session until it expires or someone logs out.

`Saved/` and **not** Project Settings, which is the obvious-looking wrong answer — `BackendUrl`
lives in a `UPROPERTY(Config)` and this looks like the same kind of thing. It is not: config
writes land in `Config/DefaultGame.ini`, which every consumer project has in git, and a session
token must never be committable. Nothing in the persistence path goes near `GConfig`.

Three things the file does:

- **It is scoped to the backend that minted it.** The REST base is stored alongside the token and
  must match on load, or the file is discarded — the persistence-time counterpart to
  `OnSettingsChanged` dropping the session when the address moves. A restart is not a way to
  smuggle a token from backend A to backend B.
- **It is loaded before the first connect.** So the very first handshake carries the bearer and
  the hub names this client the account from the outset. Loading it after `Connect()` would mean
  every launch connects as a guest and immediately reconnects, which every other client in the
  room sees as a join/leave flicker.
- **It is written in one place and deleted in one place** — `AdoptSession` and `ClearSession` — so
  logout, an expired token and a backend change are each covered without a fourth caller to
  remember. Closing the editor is not a logout: `Deinitialize` drops the in-memory copy and leaves
  the file alone.

Validation is `GET /auth/me`, reusing `Refresh Identity` rather than a second validator, and it is
triggered from the health probe's success path — the probe already retries until the backend is
reachable, so hanging the check off it means no second retry loop. A **guest** answer means the
token was revoked or expired, and it is deleted. An **unreachable** backend means nothing about
the token, so it is kept and the identity is left untouched; deleting a session because a packet
dropped would log a user out on every hiccup, and after the connection-pool finding above a
background request timing out is routine here rather than exceptional.

Between the restore and the answer, `Is Identity Provisional` is true: `Get Identity` reports the
persisted `DisplayName` with `Kind == Unknown`. Showing that name is the point — a launch does not
have to look anonymous — but it is a label read off a disk, not evidence, so anything that depends
on *being* that account must gate on `Kind == User` and never on the name being non-empty. Only
`/auth/me` promotes `Kind`. Deliberately persisted: the alternative, reporting nothing until
validation, is honest but blanks the name for seconds on every launch, and the token is the
authority while the name is only a label — so a brief stale name after a server-side rename is the
cheaper of the two costs, and `Refresh Identity` writes the corrected one back.

The user id and the admin flag are **not** persisted. `is_admin` especially: a capability read
back off disk is one that anybody with write access to `Saved/` could grant themselves in a UI.
The backend re-checks it on every admin route regardless, so caching it could only ever produce a
misleading screen.

### Being named in the room

Two small things decide what everyone else sees this client called (both HAM-176 tails).

**A suggested roster name.** The `hello` now carries `displayName` from the `GuestDisplayName`
setting, when one is set. Without it the hub names this client `Guest-` plus the first six
characters of its `clientId` — an unreadable string in every other client's roster.

It is a suggestion and never a claim. The hub clamps it to 32 characters, strips control
characters, rejects it outright if nothing survives that, and — when accounts are enabled —
rejects it if its normalized form collides with a **registered** username, because a guest may not
wear the name of an account that exists whether or not it can prove it. So the setting cannot
impersonate anyone, and the plugin does not second-guess any of those rules: it sends the value
raw and lets the hub, which is the authority, decide. The one check it does make is not sending an
empty string, which could only ever be rejected.

**Editing it applies immediately — while connected as a guest.** The name rides in the `hello` and
nothing renames a socket already up, so the subsystem reconnects to suggest the new one, the same
way `BackendUrl` reconnects for an address change. That is affordable: a reconnect re-sends the
whole scene but re-fetches no GLBs (measured — a login reconnect re-applied 33 nodes with zero
meshes rebuilt).

**While logged in it deliberately does nothing.** The hub takes the name from the session and never
consults the suggestion, so a reconnect could not change the displayed name by one character, and
the real cost of a reconnect is not the GLBs — it is that every other client in the room sees a
leave and a join in their roster. Paying that for no effect is the one case worth skipping. The
edit is not lost: the next guest connection picks it up, and logging out is one. A log line says
so at the time, rather than leaving the panel looking broken.

No other setting touches the socket. `LightIntensityScale`, `bBaseAlignModels` and
`WebAssetPrefix` are read where they are used and apply live without reconnecting anyone.

**It is ignored while logged in.** `identity_from_websocket` resolves a token first and never
consults the suggestion when one resolves, so this renames a *guest* and never an account. The
plugin sends it either way rather than branching on the token — the hub already ignores it, and a
branch would be more code for the same outcome.

This is not merely cosmetic: it is the only way this plugin can know its own guest name at all.
Inbound `clients` (the roster message) is deliberately not handled here, and `GET /auth/me` mints a
fresh random `Guest-xxxxxx` on every call, so there is nothing to read the name *back* from.
Proposing it is how it becomes knowable.

**Attribution for what a guest creates.** `POST /generate` carries `X-Client-Id` with the same
`clientId` the `hello` sends, so a guest's generations are credited to the one stable
`Guest-xxxxxx` the roster already shows them under instead of a fresh name per request. Every asset
and job records who made it, resolved by the backend rather than taken from a name a client hands
over.

That header is a **name seed only**. It proves nothing and authenticates nothing — both providers
resolve a real session first and an authenticated one always wins over it, so it can never be used
to claim `kind: "user"` or reach anyone else's session. It goes on asset-creating requests as the
backend documents, not on every REST call: that is the only place it means anything, and a header
that travels further than its purpose invites being mistaken for one that matters.

## Reporting local selection

Every client tells the room what it has selected so the others can draw a border on those nodes in
its colour. The normative contract is `docs/scene-format.md`, *"What each client has selected — the
`selection` message"*; this is only how it lands in Unreal.

| Blueprint node (`Looma\|Selection`) | What it does |
| --- | --- |
| `Set Local Selection` | Replace the whole selection. The canonical call — the wire carries a whole set, so this mirrors it |
| `Select Node` / `Deselect Node` | Add or remove one node, read-modify-write over the above |
| `Clear Selection` | Select nothing, which **sends** `{"ids": []}` — that is what clears the borders |
| `Get Local Selection` / `Get Local Selection Ids` / `Is Node Selected` | Read it back; destroyed actors are already gone from it |
| `On Local Selection Changed` | Fires once per local change, carrying the ids. About local truth, not the wire: it fires while disconnected too, and does *not* fire when a reconnect merely re-tells the hub |

Three things about it are worth knowing, because each one is a rule from the contract rather than a
choice:

- **`[]` is a real message, not a no-op.** There is no teardown message and nothing retracts a
  claim, so clearing the selection has to send. Only the diff decides whether a *particular* call
  results in traffic.
- **The send is coalesced into the tick, and diffed.** An idle scene sends nothing; a gesture that
  clears and then adds three nodes in one frame sends one message rather than four, so no other
  client ever draws the intermediate states.
- **`ids` is all this client sends.** The hub stamps `clientId` and `color` from its own presence
  table and discards anything a client puts in them, so a client cannot wear another's colour or
  file a selection under someone else's name.

A selection is also re-sent on every connect. A reconnecting client comes back with an empty
selection as far as the hub is concerned — the socket that made the old claim is gone — so the
send-on-connect is what restores the borders. That fires more often than it sounds: a login and a
logout each reconnect.

### Selecting in the editor

In the editor, the plugin also mirrors **the editor's own actor selection** while PIE is running:
click a synced node in the World Outliner and a border appears on it in every other client. It runs
through the same `Set Local Selection` as everything else, so there is one implementation and one
diff.

**Press F8 to eject first.** Until you do, the PIE Outliner will not let you select runtime actors,
and the feature looks broken when it is only unreachable. This is the first thing to try if nothing
seems to happen.

Two behaviours worth knowing, because both are deliberate:

- **It does nothing before you press Play**, and not because of a filter. `ULoomaSceneSyncSubsystem`
  is a `UGameInstanceSubsystem`, so it does not exist outside a running game — dressing a level at
  design time is not a claim about what anyone is working on.
- **Selecting a non-synced actor clears the selection** rather than being ignored. If you click a
  stray light, you have stopped working on the node you had, and a border nobody ever retracts is a
  false claim left in everyone else's viewport. Keeping the old selection would leave it there until
  you happened to click a synced node again.

The hook is editor-only — `UnrealEd` is a `Target.bBuildEditor` dependency and the code is behind
`WITH_EDITOR`. In a packaged build the API and the console commands are the whole feature, which is
why they are the half that exists.

## Remote selections

The inbound twin of the section above: everyone else's selections, drawn in their own colours so a
shared scene reads as shared. The normative contract is `docs/scene-format.md`, *"Who else is in the
room — the `clients` message"* and *"What each client has selected"*; this is how it lands in
Unreal.

Two messages feed it. `clients` is the whole room, re-sent on every join and every leave, carrying
each client's colour, name, kind and current selection. `selection` is one client's whole set,
changing hands between rosters. Neither is scene state: **none of it is merged into the document,
sent in a `scene`, or saved, and all of it dies with the socket** — there is no teardown message, so
dropping a client that has left the roster is the only thing that clears its borders.

| Blueprint node (`Looma\|Presence`) | What it does |
| --- | --- |
| `Get Clients` | The **other** clients, in roster order (= join order). Empty while disconnected |
| `Get Own Client Id` | Our own id in the room — the roster's `you` |
| `Get Client` | One client by id, **ours included**; the self entry is held apart from the list but is still findable |
| `Get Node Border Owner` / `Get Node Claimants` | Who wins a node's border, and everyone claiming it oldest-first |
| `Get Client Border Nodes` | The nodes one client both selected *and* won — one outline group's worth |
| `Get Remote Border Groups` | The whole draw list: per client, its colour, stencil slot, own nodes and descendants |
| `Get Undrawn Clients` | Clients holding a border that no stencil slot was left for |
| `On Clients Changed` | The room moved — a join, a leave, or somebody's selection. Fires only on a real change, and fires with an empty array when the socket drops |

`Get Own Client Id` closes a gap worth naming: **this is the only way the plugin can learn its own
room name.** `GET /auth/me` mints a fresh random `Guest-xxxxxx` on every call when no session is
held, so that name matches nothing anyone else sees; the roster's self entry is the real one. Set
`GuestDisplayName` to have something readable there — see *Being named in the room*.

Five rules govern what is drawn, and each is a rule from the contract rather than a choice:

- **First claim wins, and nothing is locked.** Two people can select one node and both can still
  edit it; this is a drawing rule only, so that one node has one border. The claim ledger is
  `nodeId → claimants, oldest first`, and the head draws. Every client receives hub messages in the
  same order, so each maintains that order locally and they all agree with no arbitration. A real
  per-selection lock is planned and will replace it.
- **Your own selection always wins on your own screen.** Our ids are subtracted from every remote
  border first, thick and thin alike, so a remote claim can never make us lose track of what we
  hold. Our own colour is what *other* people see for us; it is never drawn here.
- **A node someone claimed outright is never another client's descendant hint** — the same
  precedence a selection has over a child hint. One node, one border, however many claimed subtrees
  it sits under.
- **A hint follows the nearest owning ancestor, and stops at a claimed node rather than passing
  through it.** Take `P → C → G`, where X claims `P` and Y claims `C`. `C` is Y's outright claim, so
  the rule above already keeps X's hint off it — but `G` is not claimed by anyone, and the two
  possible answers differ. Here the walk from `P` stops at `C` and never reaches `G`, so `G` becomes
  **Y's** hint: a hint means "this moves with the thing above it", and the nearest thing above `G`
  that anybody holds is Y's. Letting X's colour reappear two levels below Y's claim would read as X
  holding a subtree that is not theirs.

  A known difference from the web client, which builds each subtree in full and filters afterwards
  (`descendants()` in `tree.js`), so `G` there becomes **X's** hint — the hint leapfrogs Y's claim.
  Not a contract violation on either side: descendant hints are a local rendering convention and
  appear nowhere on the wire, so no client is wrong by it. Filed for a doc pass alongside HAM-196.
- **An unknown node id is kept, not filtered.** A `selection` legitimately races the `spawn` that
  created its node, and nothing re-sends a dropped claim, so the claim is held and the border simply
  appears if the node arrives.
- **An unknown `clientId` is drawn in a neutral grey (`#bbbbbb`), never guessed and never dropped.**
  The roster and a `selection` are fanned out independently, so a selection can land just before the
  roster that introduces its sender. The next roster corrects it.

Two limits are deliberate and visible rather than silent:

**The budget is eight clients** (`LoomaRemoteBorderSlots`). Not an engine limit — the stencil
encoding holds 127 — but the hub's colour palette is eight and **repeats** past that
(`docs/scene-format.md`), so a ninth border would be drawn in a colour somebody else is already
wearing. An ambiguous border is a wrong answer where a missing one is a visibly missing one. Clients
past the budget are named by `Get Undrawn Clients`, logged once each time the list changes, and
printed by `Looma.Claims` — show them in your UI, because a client that is present, working and
invisible is otherwise indistinguishable from an empty room. Slots are spent in roster order, one
per client that actually won a claim, so a client holding nothing costs nothing.

**A node with no primitive draws nothing.** A `light` node and an empty group node have no mesh to
outline, so a claim on one is held in the ledger, counts toward its owner's group, and produces no
border. The web client solved this with a pick proxy (HAM-148); this plugin has no equivalent,
because it can ship no assets. If you need it, spawn your own proxy primitive as a child of the
node — it will be picked up automatically, since every `UPrimitiveComponent` on a synced actor is
marked.

`Looma.Room` prints the roster with our own entry marked; `Looma.Claims` prints the ledger both ways
round — each claimed node with its claimants, then each client with how much of its selection it
actually wins — plus which stencil slots are in use. Those numbers differ the moment anyone is
contested, and telling a lost tiebreak from a broken border is the whole reason both are printed.

### Wiring the outline

**The plugin marks; it does not draw.** It sets `CustomDepthStencilValue` on the primitives whose
borders you should see and publishes the colours; a post-process material in your project turns the
two into an outline. That split is forced — `LoomaSceneSync.uplugin` sets `"CanContainContent":
false`, so the plugin can ship code and nothing else, no material and no parameter collection — and
it is also the right shape, since a project that already has an outline material wires this in
without adopting a second one.

Four things to build, in order:

**1. Turn on the custom depth stencil.** *Project Settings > Rendering > Postprocessing > Custom
Depth-Stencil Pass* = **Enabled with Stencil** (`r.CustomDepth=3`). Without it the stencil buffer is
never written and **nothing appears at all**, however correct everything else is. The plugin cannot
set this for its host — writing to a project's render config from a plugin would be a worse surprise
than a warning — so it checks the CVar the first time a border exists and logs a warning naming the
value it found.

**2. Create a Material Parameter Collection** anywhere in your content, with:

| Parameter | Type | Meaning |
| --- | --- | --- |
| `LoomaClient1` … `LoomaClient8` | Vector | Slot *n*'s colour, linear. **Alpha is the occupancy flag** — 1 when the slot is in use, 0 when it is free |
| `LoomaClientCount` | Scalar | How many slots are in use |

Point *Project Settings > Plugins > Looma Scene Sync > Remote Selection Parameter Collection* at it.
Every slot is rewritten on every change, empty ones included: writing only the occupied slots would
leave a departed client's colour sitting in the collection for a material to draw with, which is the
"border in the wrong colour" failure — strictly worse than none. Leave the setting empty and the
plugin logs once that stencil values are being written with no colours published, because "not
configured" and "broken" otherwise produce the identical symptom.

**3. Decode the stencil in a post-process material** (blendable location *Before Tonemapping*), from
`SceneTexture:CustomStencil`:

| Value | Meaning |
| --- | --- |
| `0` | No border. Reserved, and never allocated — a stray non-zero value can never be mistaken for a claim |
| `1` … `8` | Slot *n*, **thick** — a node that client selected and won |
| `129` … `136` | Slot *n*, **thin** — a descendant of one of those nodes, the "this moves with it" hint |

So: `IsChild = Stencil > 128`, `Slot = Stencil - (IsChild ? 128 : 0)`, then look up `LoomaClient<Slot>`
and pick an edge weight. A high bit rather than `slot * 2 + weight` because the decode is then a
compare and a subtract rather than a floor and a modulo — and the thick values stay equal to the
slot number, which makes a stencil buffer readable in RenderDoc without decoding anything.

Match the web client's weighting so the two viewers agree: thick edges at strength 5, thin at 2
(`frontend/src/scene/Scene.jsx`), and dim the occluded half of an edge rather than colouring it
differently, so a claim on something behind another object still reads as that client's.

**4. Or skip 2 and 3 entirely.** `Get Remote Border Groups` hands you the same decision — client,
colour, slot, own nodes, descendants — as plain Blueprint data, recomputed whenever the room, the
local selection or the scene moves. Drive your own materials from it if the collection route does
not suit; the stencil is written either way.

The plugin deliberately does **not** drive a stencil for your own local selection. That is your
project's business and it must not change when the room does — which is the point of the
your-selection-always-wins rule above.

## Status

Beta (`IsBetaVersion: true`), version 0.1.0.
