# HAM-159 handoff

Updated 2026-09-14. Plugin implementation, independent review and desktop cross-client
rendering acceptance are complete on
`chadjiminaschrysostomos/ham-159-unreal-remote-selection-colours`, in
[plugin PR #5](https://github.com/CYENS/looma-scene-sync-plugin/pull/5).
The remaining gate is user-controlled merge and the consumer-pin sequence. This is part of
[HAM-153](https://linear.app/hamlet-loomaxr/issue/HAM-153); Unity is outside its scope.

## Changes

| Commit | Behavior |
| --- | --- |
| `9aa0186` and earlier PR commits | Inbound client roster, claim ledger, colour slots, own/descendant stencil assignment and Blueprint accessors |
| `010bafb` | Scene upserts, component patches and remote/local reparenting invalidate existing borders; four stencil regressions |
| `67a5d31` | Colour-only and provisional-client notifications, post-refresh rendering event, scene identity cleanup and four coherence regressions |

`OnClientsChanged` remains immediate and reports client data. Custom renderers should bind
`OnRemoteBordersRefreshed`, then read `GetRemoteBorderGroups` and `GetUndrawnClients`.
That event runs after stencil assignment and colour publication, once per dirty tick. It may
report identical group data after a primitive replacement; unchanged frames do not emit it.

An actual `(performance.id, sceneId)` change clears remote claims and local selection, including
when the new scene reuses existing node ids/actors. A same-identity snapshot preserves both;
the first scene frame preserves presence that arrived before it. Presence still resets on socket
teardown. Empty scene ids are valid identities, so a separate initialization flag distinguishes
the first frame from a transition to an unsaved scene.

## Verification

Built the Development Editor target with the umbrella's
`code/looma-plugin-test-project/build.ps1` against installed UE 5.8. Ran the plugin's
`Looma.Presence` automation group in `UnrealEditor-Cmd` with `-nullrhi`; all eight cases passed
with zero test warnings or failures. The README gives the command and report check.

The first four tests failed against the pre-fix scene-edit behavior and passed after `010bafb`.
The four new coherence tests failed against `010bafb` while the first four remained passing.
The final run passed all eight. Local evidence is under this plugin's ignored
`Saved/Automation/SceneRefreshBefore`, `SceneRefreshAfterClean`, `CoherenceBefore` and
`CoherenceAfter`, each with an `index.json` and an adjacent `.log`.

Tests feed real JSON frames and inspect actual primitive stencils/custom-depth flags and public
getters. A reflected observer binds the real dynamic delegates and reads the cache and stencil
inside the refresh callback. The fixture uses a transient world and does not initialize the
subsystem, restore sessions or connect to a backend. Its narrow friend access seeds tracked
actors and exercises the existing outbound attachment diff and presence teardown directly.

The CPU suite is complemented by the desktop rendering and live WebSocket acceptance below.

## Desktop integration acceptance — passed

The reviewer exercised the actual UE 5.8 viewer's `LVL_Demo` in Simulate In Editor with
D3D12, the committed outline volume and an isolated live backend. Playwright exercised the
web client, with additional live peers for conflicts and join/leave transitions. The active
scene was `ham-153-presence-acceptance`.

Local evidence is in the umbrella's ignored `.worktrees/ham-153-validation/`. The JSON
snapshots record connected state, local selection, remote clients, border groups, primitive
stencils and all eight live material-collection vectors.

| Check | Observed result | Evidence in that directory |
| --- | --- | --- |
| Web → Unreal, descendants and colour | Web colour `#6047e1` matched the group's linear colour and `LoomaClient1`. Parent stencil was 1, child/grandchild 129, unrelated object/occluder 0. The active view rendered the outline. | `ue-inbound-evidence.json`, `ue-inbound.png` |
| Unreal → web | Selecting `ham153-other` in Unreal appeared in the web Outliner and presence roster in Unreal's colour `#47e160`. | `ue-outbound-evidence.json`, `web-unreal-selection.png` |
| Local priority | Selecting the parent locally suppressed all remote groups, stencils and collection slots. | `ue-local-priority-evidence.json` |
| First claim, unrelated join and handover | B selected before A and retained ownership when unrelated C joined; B deselecting promoted A. | `ue-contested-evidence.json`, `ue-unrelated-join-evidence.json`, `ue-handover-evidence.json` |
| Peer disconnect | Closing A cleared remote groups, primitive stencils and collection slots. | `ue-peer-disconnect-evidence.json` |
| Scene switch with reused ids | Switching to `ham-153-scene-reset` reused all five node ids and cleared claims/render state; returning to the acceptance scene hydrated A's current selection. | `ue-scene-reset-evidence.json`, `ue-scene-return-evidence.json` |
| Reconnect and rehydration | `Looma.Reconnect` reconnected with cleared presence. Explicitly reopening the acceptance scene hydrated the current claim and restored its stencils/colour. | `ue-reconnect-evidence.json`, `ue-reconnect-hydration-evidence.json` |

The reconnect check exposed existing scene-navigation behavior: reconnect opened the default
`untitled-scene`, rather than preserving the previously opened scene. Running
`Looma.Scene ham-153-presence-acceptance` then rehydrated presence correctly. This acceptance
does not promise scene preservation across reconnect; same-scene resync preservation remains
covered by the automation test.

Verification scope was the desktop editor runtime, not a packaged build or XR hardware.
All test UE processes were stopped and the user's `DefaultGame.ini` restored. For repeat runs,
use launch overrides targeting the actual `LoomaSceneSyncSettings` class and an isolated
`-saveddirsuffix`: changing the settings CDO can persist values to project configuration.

## Host setup and merge handoff

The host assets and active-view attachment belong to
[viewer PR #2](https://github.com/CYENS/looma-xr-viewer-ue58/pull/2), tracked as HAM-209.
The plugin ships no assets. UE 5.8 calls the intended material location **Scene Color After
DOF** (`BL_SceneColorAfterDOF`); follow the current README rather than the old Before Tonemapping
label. The material collection needs eight `LoomaClientN` vectors (alpha is occupancy) and
`LoomaClientCount`; thick stencils are 1–8, descendant stencils 129–136.

Restart the editor after rebuilding: the new reflected delegate cannot be safely added with
Live Coding. Web roster reconciliation is tracked separately in HAM-196. The independent
source review and desktop integration gate are complete; the evidence above records what ran.

Merge only after explicit user approval. The dependency order is plugin PR #5, viewer PR #2
with its reviewed plugin pin, then the umbrella's consumer pins. No merge or pin update is
part of this handoff.
