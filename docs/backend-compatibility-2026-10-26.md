# Backend compatibility review — 2026-10-26

Compared the plugin's latest merged `main`, `06b5cda`, with
`looma-xr-asset-demo` at `77c41b9` (`origin/develop`); backend `origin/main`
is `241eb5f` and includes the component changes discussed here. The requested
plugin checkout was fast-forwarded from `e021b41` before making this patch.
The backend checkout has local skybox work; it was inspected but left untouched.

## Changes selected

1. **Honour `enabled: false`.** Backend commit `0418ce1` added the component
   switch. Previously Unreal ignored it. A literal JSON false now omits that
   component from the active runtime specs, using the existing removal paths to
   stop rendering, lighting, sound and embedded animation. A disabled Live Link
   component exposes no active subject/source/stream. Other values, including
   missing, remain enabled, as the wire contract requires. Whole wire components
   remain on the actor for later edits and re-enabling; children are unaffected.
2. **Keep the pulled audio code building in UE 5.6.** `ISoundGenerator` in the
   installed 5.6 headers has no `GetNumChannels` virtual, so the new unconditional
   override prevented compilation in the requested host. Compile it for 5.8 and
   later; 5.6 uses the channel count already set on `USoundWave`. No audio format
   or playback changes are needed.

The parser fix adds no wire format, REST endpoint, asset or Blueprint member.
Disabled model/audio components use the existing removal/recreation behaviour;
re-enabling can rebuild their runtime objects. No invisible picking proxy is
added: disabled objects remain accessible through the editor Outliner. Raw live
pose getters remain a transport API; a consumer driving a skeleton must consult
the active Live Link subject, as before.

## What the pull already supplies

The 19 pulled commits include rigged GLB loading and embedded clips, the throttled
model queue, live pose publication/reception and buffering, hub Live Link
source/stream selection, sound playback, a disk GLB cache, and generation meta.

Backend commit `e6e3ad7` now serves optimized models at the **same** `/static/*.glb`
URL. The requested checkout uses `CYENS/glTFRuntimeLoomaXR` and its parser already
handles `EXT_meshopt_compression`. This needs no new request path or decoder.
Rigs/motions are excluded from backend optimization. Rendering an optimized
catalog asset remains a useful visual acceptance check.

## Recommended next work, without duplicating existing branches

| Priority / trigger | Gap and evidence | Suggested scope |
| --- | --- | --- |
| Host setup for catalog textures | `looma-xr-plugin-development/Plugins` contains `glTFRuntime` but no `glTFRuntimeWebP`. The umbrella guidance requires WebP support for datalake textures. | Install/enable the `CYENS/glTFRuntimeWebPLoomaXR` fork in the host. The harness checks below do not verify WebP texture decoding. |
| Next for scene parity | `hidden` is authored by backend/web commit `a03b502`; plugin `main` still skips it. | Review existing [PR #20](https://github.com/CYENS/looma-scene-sync-plugin/pull/20), which implements subtree visibility and collision, preserving audio. It is open, not part of the pulled main. |
| Before using Unreal as a contributor or editing locked objects | Backend `7945400` enforces `added.owner`, `lock`, and contributor permissions. The plugin sends local edits and `HandleSceneError` only logs a refusal; it does not restore a rejected local move/delete. | Expose current edit access/ownership and reconcile a refused local mutation. Keep the backend authoritative. Test locked edits and contributor edits to somebody else's node. |
| Before expecting motion playback to match the browser | The animation contract includes external `motion`, `loop`, `speed`, `startedAt`, `offset`, `seek` and hub clock messages. `ParseAnimation` currently reads only `clip` and `playing`; `ApplyAnimation` loops locally and returns to rest on pause. | Separate motion/timeline task, beginning with pause/resume and synchronized embedded clips; add external motion loading only when needed. |
| Before comparing captured takes across clients | Backend `e935a93` records a pose's capture timestamp. | Review existing draft [PR #18](https://github.com/CYENS/looma-scene-sync-plugin/pull/18); do not implement the timestamp a second time. |
| When remote head/hand presence is needed | The backend already relays `clientPose`; it is not in plugin main. | Continue existing draft [PR #17](https://github.com/CYENS/looma-scene-sync-plugin/pull/17). |
| After the backend contract settles | Local uncommitted `skybox` component, schema, ingestion and client changes. | Defer Unreal rendering until the asset representation and scene semantics are committed. Unknown components continue to be retained. |

Administrative UI, generation-provider configuration, photo reconstruction,
recording controls and the web VCam navigation are backend/web features, not
requirements for a basic Unreal scene viewer. They are excluded from this update.

## Verification

Use isolated harnesses so the user's running 5.8 editor and the other plugin
checkouts are unaffected. `Looma.Components.Enabled` covers strict switch values
on all seven supported types, retained wire payloads, and real patch dispatch
switching meshes/lights off and on without hiding the child or respawning nodes.

| Check | Result |
| --- | --- |
| UE 5.6 Development Editor build, core + UI + requested glTFRuntime checkout | Passed. The unmodified pulled audio code first failed on the missing virtual; the guarded version builds. |
| UE 5.8 Development Editor build, same sources | Passed. Existing JSON deprecation warnings originate in glTFRuntime's editor exporters. |
| New `Looma.Components.Enabled` tests against the old parser | Both failed, demonstrating the regression. |
| Focused tests on UE 5.6 after the fix | 19 successes, zero failures/warnings. |
| Same focused tests on UE 5.8 after the fix | 19 successes, zero failures/warnings. |

The focused selection was `Looma.Components.Enabled`, `Looma.Presence`,
`Looma.LiveLink`, `Looma.Audio.Parse`, `Looma.Audio.DecodeWav`,
`Looma.Audio.Attenuation`, and `Looma.Audio.SoundWave`. Results were read from
the exported `index.json`, not inferred from the editor process exit code.
Harnesses and reports are in the user's temp directory as
`looma-scene-sync-backend-ue56-20261026` and
`looma-scene-sync-backend-ue58-20261026` (`Reports/Compatibility/index.json`).
Tests ran headless with `-nullrhi -nosound`: visual rendering, audible playback,
and the full mocap host build were not exercised. Rebuild the actual 5.6 host to
use the updated source; its existing DLLs were not replaced by the harnesses.

The normative references are the backend's `docs/scene-format.md` sections
"Switching a component off", "The hidden component", "Contributors and locks"
and animation playback; implementation references are `backend/app/scenegraph.py`,
`backend/app/sync.py`, and `backend/app/main.py` (`RevalidatedStaticFiles`).
