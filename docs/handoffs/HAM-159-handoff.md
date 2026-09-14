# HAM-159 handoff

Updated 2026-09-14. Plugin implementation is prepared on
`chadjiminaschrysostomos/ham-159-unreal-remote-selection-colours`, in
[plugin PR #5](https://github.com/CYENS/looma-scene-sync-plugin/pull/5).
Independent review and cross-client rendering acceptance remain gates. This is part of
[HAM-153](https://linear.app/hamlet-loomaxr/issue/HAM-153); Unity is outside its scope.

## Changes

| Commit | Behavior |
| --- | --- |
| `9aa0186` and earlier PR commits | Inbound client roster, claim ledger, colour slots, own/descendant stencil assignment and Blueprint accessors |
| `010bafb` | Scene upserts, component patches and remote/local reparenting invalidate existing borders; four stencil regressions |
| This change | Colour-only and provisional-client notifications, post-refresh rendering event, scene identity cleanup and four coherence regressions |

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

These are CPU ownership/notification checks. They do not verify the host material's pixels,
the live collection's GPU values or a real web-to-Unreal WebSocket session.

## Integration still required

The host assets and active-view attachment belong to
[viewer PR #2](https://github.com/CYENS/looma-xr-viewer-ue58/pull/2), tracked as HAM-209.
The plugin ships no assets. UE 5.8 calls the intended material location **Scene Color After
DOF** (`BL_SceneColorAfterDOF`); follow the current README rather than the old Before Tonemapping
label. The material collection needs eight `LoomaClientN` vectors (alpha is occupancy) and
`LoomaClientCount`; thick stencils are 1–8, descendant stencils 129–136.

Restart the editor after rebuilding: the new reflected delegate cannot be safely added with
Live Coding. Then verify two browser clients and the viewer together, including contested
selections, local priority, descendant hints, reparent/component edits, roster joins/leaves,
deselection, reconnect and a scene switch. Confirm visible colours and occluded edges from
the active camera. Web roster reconciliation is tracked separately in HAM-196.

Merge only after explicit user approval and the named acceptance gate passes or is explicitly
waived. The dependency order is plugin PR #5, viewer PR #2 with its reviewed plugin pin, then
the umbrella's consumer pins. No merge or pin update is part of this handoff.
