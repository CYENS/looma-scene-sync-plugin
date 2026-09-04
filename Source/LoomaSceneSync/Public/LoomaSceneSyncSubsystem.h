#pragma once

#include "CoreMinimal.h"
#include "LoomaAuthTypes.h"
#include "LoomaGenerationTypes.h"
#include "LoomaPresenceTypes.h"
#include "LoomaSyncedActor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "LoomaSceneSyncSubsystem.generated.h"

class IWebSocket;
class IHttpRequest;
class FJsonObject;
class FJsonValue;
class ULoomaGenerationHandle;
class UMaterialParameterCollection;
class UPrimitiveComponent;

/**
 * The performance — the workspace — this socket is in, as the `scene` frame reports it
 * (`_performance_info`, backend/app/sync.py).
 *
 * It rides on that frame because the hub may put a client somewhere it did not ask for:
 * one that names no performance is returned to whichever its `clientKey` was last in.
 * That is the entire reason the object exists. A client that assumed it got what it
 * asked for would show the wrong name, create scenes into the wrong workspace and read
 * the wrong chat while its socket sat in the right one — a disagreement no client could
 * detect, because nothing else on the wire ever names the performance.
 *
 * One struct rather than three getters, because the three fields are one frame's
 * snapshot and are only ever true together — and because `Name` cannot be read without
 * `Id` beside it: an empty name means "the row behind THIS id is gone", which is not a
 * statement either field can make alone.
 */
USTRUCT(BlueprintType)
struct FLoomaPerformance
{
    GENERATED_BODY()

    /**
     * Where the socket actually is. Always on the frame and always true, including when
     * the row it names has been deleted — which is what makes it the field to route on
     * and to compare against, and the only one a REST path may be built from.
     *
     * Empty means one thing and nothing else: no `scene` frame has arrived yet. It is
     * not a state the hub can put us in.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma")
    FString Id;

    /**
     * The workspace's name, for a badge or a log line. **Empty is a real answer, not a
     * missing one**: the hub sends `name: null` for a row that has gone, because
     * `_performance_info` reads `db.get_performance(id) or {}`. So an empty name beside
     * a valid id says the performance was deleted out from under this socket, which is
     * still in it and still working.
     *
     * It cannot mean "the name is blank". `performances.create` and `performances.update`
     * both refuse a name that strips to nothing, so a stored name is never empty; the
     * null/"" distinction FString cannot hold is one the backend cannot produce.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma")
    FString Name;

    /** `public` or `private`. The hub falls back to `public` for a row that has gone. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma")
    FString Visibility;
};

/**
 * One row of `GET /scenes` — the metadata list, which is the only route that answers
 * "what scenes are there" without dragging a node document per answer.
 *
 * Just the two fields a resolver needs. The row carries `node_count`, `owner`,
 * `visibility`, `public_role`, `performance_ids` and `role` as well, and this struct is
 * where they go when something needs them — a listing command will — but parsing fields
 * nothing reads is inventing a contract to keep in step for no caller.
 *
 * A plain struct and not a USTRUCT, unlike FLoomaPerformance. Nothing Blueprint-facing
 * consumes it: the fetch it comes from is asynchronous through a TFunction, which
 * Blueprint cannot express, so exposing the struct alone would publish half a feature.
 * Promoting it is the easy move on the day something needs it.
 */
struct FLoomaSceneSummary
{
    FString Id;
    FString Name;
};

/**
 * One row of `GET /performances` — what workspaces exist for this caller, and what it
 * may do in each.
 *
 * A SECOND performance struct beside FLoomaPerformance, deliberately, because they come
 * from different producers and disagree about what a performance is. FLoomaPerformance
 * is the `scene` frame's object: where this socket IS, carrying `visibility` and a name
 * that goes null when the row is deleted. This is a catalogue row: what exists, plus
 * `role` — the caller's own standing, which the frame never carries and which is
 * per-caller rather than a property of the row at all. Merging them would make a struct
 * whose fields are empty or not depending on which half filled it in, and every reader
 * would have to know which.
 *
 * The row also carries owner, visibility, state and timestamps. Same rule as
 * FLoomaSceneSummary: this struct is where they go when something reads them.
 */
struct FLoomaPerformanceSummary
{
    FString Id;
    FString Name;
    /** `editor` or `viewer` — what THIS caller may do here. Empty if the row omitted it. */
    FString Role;
};

/**
 * One cue of a performance's running order — `GET /performances/{id}/cues`.
 *
 * THE POOL AND THE RUNNING ORDER ARE DIFFERENT LISTS, and this is the second one.
 * `performance_scenes` is the set of scenes a performance is working on;
 * `performance_cues` is the chronology, what happens and in what order. A scene can sit
 * in the pool without being on the line — a draft, a cut scene, an alternate take — so
 * an empty running order is an unarranged performance and not a broken one
 * (backend/app/performances.py, "The pool and the running order are different lists").
 *
 * A SCENE MAY APPEAR TWICE. That is a reprise, and it is why a cue carries its own
 * `cue_id`: keying cues on (performance, scene) would have quietly forbidden it. The
 * consequence here is that scene id -> cue is one-to-many, so "which cue are we on"
 * cannot be answered by matching the active scene against this list — see LogCues.
 */
struct FLoomaCue
{
    /**
     * The scene this cue fires. A cue's identity on the wire is its `cue_id`, which is
     * deliberately not parsed: nothing in this console addresses a cue by id — the
     * index is the handle a person types — and `cue_id` is what a caller that REORDERS
     * or relabels the rail needs. It goes here on the day something does.
     */
    FString SceneId;
    /** Optional and often absent: `add_cue` stores `clean_label or None`. */
    FString Label;
};

/** Per-node bookkeeping for the outbound motion diff. */
struct FLoomaTrackedActor
{
    TWeakObjectPtr<ALoomaSyncedActor> Actor;
    /**
     * Parent-local, like everything on the wire — a child's diff must not see its
     * parent's motion. The world pose while the actor hangs off something the wire
     * cannot name, which is the pose we report for it (see TickOutbound).
     */
    FTransform LastSent;
    int32 StillFrames = 0;
    bool bMoving = false;
    /**
     * Attached to an actor that is not a node, as of the last poll. Held only so the
     * warning about it fires on the rising edge — once per attachment, not per tick.
     */
    bool bForeignParent = false;
};

/** Fired for generation-job lifecycle changes. Carries the full job snapshot. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaGenerationJobEvent, const FLoomaGenerationJob&, Job);

/** Fired when the scene-sync socket connects / disconnects. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FLoomaSyncConnectionEvent);

/**
 * Whether the backend wants a login — read from the `auth` block of `GET /health`,
 * the discovery endpoint HAM-172 added for exactly this question. The web frontend
 * branches on the same block to decide whether to draw a login screen at all, so
 * there is no second "what does this backend support" route to keep in sync.
 *
 * Three states rather than a bool, because "this backend needs no login" and "we have
 * not managed to ask yet" call for opposite behaviour: the first says go straight into
 * the scene, the second says wait. Collapsing them into `false` builds a client that
 * silently skips the login screen for the first second of every launch — and forever
 * against a backend that was not up yet — a bug that presents as a race and is really
 * a missing state.
 *
 * Registration is folded into the enabled cases instead of riding alongside as a
 * second flag, because `auth.registrationEnabled` only means anything when auth is on.
 * A standalone bool would force every call site to disambiguate "false because
 * registration is closed" from "false because there is nothing to register with".
 */
UENUM(BlueprintType)
enum class ELoomaAuthState : uint8
{
    /** Not asked yet, unreachable, or an answer we could not read. Assume nothing. */
    Unknown,
    /** `auth.enabled == false` — every route is open and no token is needed. */
    Disabled,
    /** Login required; `auth.registrationEnabled == false`, so accounts are made server-side. */
    EnabledRegistrationClosed,
    /** Login required, and this client may also create an account. */
    EnabledRegistrationOpen
};

/** Fired when the backend's auth state becomes known, or changes. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaAuthStateEvent, ELoomaAuthState, AuthState);

/**
 * Fired when *who we are* changes — a login, a logout, a `/auth/me` refresh.
 *
 * Deliberately a separate event from FLoomaAuthStateEvent above, which answers "what
 * does this backend require". The two move independently and a UI needs both: a
 * backend can demand a login (ELoomaAuthState::Enabled*) while we are still a guest,
 * and that pair of facts is exactly the state a login screen exists to resolve.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaIdentityEvent, const FLoomaIdentity&, Identity);

/**
 * This client's local selection changed. Carries the node ids now selected.
 *
 * About **local truth**, and deliberately not about the wire. It fires whether or not
 * the socket is up, because the selection changed either way — and with a login and a
 * logout each reconnecting (HAM-181), "briefly disconnected" is ordinary operation, not
 * an edge case, so a UI that went quiet during it would sit on a stale count. It
 * equally does *not* fire merely because the hub had to be told again on reconnect:
 * that is the hub's knowledge changing, not ours.
 *
 * Fires once per coalesced change, from the per-tick change detector rather than from
 * each of SetLocalSelection / SelectNode / DeselectNode. Same reasoning as the send: a
 * gesture that clears and then adds three nodes in one frame is one logical change, and
 * a UI told about it four times has to de-bounce what this can simply not emit. Being
 * a detector rather than a set of notify calls is also what catches the second way a
 * selection changes — an actor destroyed while selected, where no setter runs at all.
 *
 * The cost is that a highlight drawn from this event lags the call by up to a frame; a
 * UI that cannot accept that should read GetLocalSelection() directly, which is always
 * current.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaSelectionEvent, const TArray<FString>&, NodeIds);

/**
 * The room moved: a client joined, left, or arrived holding a different selection.
 * Carries the **remote** clients in roster order — never our own entry, for the
 * reason GetClients() gives.
 *
 * Fires only when the roster actually differs from the one we held, the same rule
 * OnAuthStateChanged and OnIdentityChanged follow: binding is then enough and nothing
 * has to poll or diff defensively. The hub re-sends the whole roster on every join
 * and every leave, so the traffic is low enough that firing unconditionally would
 * have been affordable — the objection is not cost. It is that a delegate which fires
 * when nothing moved teaches every consumer to diff before acting, and step 3's
 * consumer rebuilds border geometry, which is exactly the work not worth doing twice.
 *
 * It DOES fire with an empty array when the socket drops, and that is not a
 * formality: presence dies with the socket, and this event is the only thing that
 * will ever tell a consumer to take those borders down. There is no teardown message.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaClientsEvent, const TArray<FLoomaClient>&, Clients);

/**
 * Client of the LoomaXR scene-sync hub (backend /ws/scene), speaking **scene format
 * v3** — the normative contract is looma-xr-asset-demo/docs/scene-format.md.
 *
 * A scene is a flat list of nodes with parent pointers: `{id, parent, name, t,
 * components?}`. There is one kind of node — an object with a transform — and what it
 * renders comes from its components, so every node becomes one ALoomaSyncedActor
 * attached to its parent's actor, and `t` is applied as a **parent-local** transform.
 *
 * Inbound: hello -> `scene` (the whole document, on connect and on activation), then
 * `spawn` / `despawn` / `transform` / `reparent` / `patch`. Structural ops are
 * re-broadcast by the hub from its own normalised state to *every* client including
 * the sender, so all of them are applied idempotently. `clients` rides alongside as
 * presence rather than scene state — the room roster, never merged into the document.
 *
 * Outbound: every tick, each tracked actor's parent-local transform is diffed against
 * a last-sent cache — any motion (editor/PIE gizmo, physics, sequencer, code) streams
 * as transient transforms at <=30 Hz, with one final commit after the actor comes to
 * rest. The same poll diffs each actor's *attachment* against the parent the hub last
 * told us, so re-parenting in the World Outliner reports a `reparent`, ahead of any
 * pose for that node. Works in packaged builds (no editor delegates).
 *
 * Wire convention: right-handed, Y-up, meters, quaternion [x,y,z,w]
 * (three.js/glTF native). Conversion to UE (left-handed, Z-up, cm) is
 * (x,y,z) -> (-z,x,y) x100, matching glTFRuntime's DEFAULT SceneBasis
 * (FglTFRuntimeConfig::GetMatrix) so meshes and transforms agree. It stays valid
 * per node under hierarchy: a change of basis composes across a parent chain
 * (M(L1 L2)M^-1 = (M L1 M^-1)(M L2 M^-1)), so each node's local transform converts
 * independently and UE attachment composes the world pose as three.js does.
 */
UCLASS()
class LOOMASCENESYNC_API ULoomaSceneSyncSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    // --- UGameInstanceSubsystem ---
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // --- FTickableGameObject ---
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override;

    /**
     * Spawn a datalake asset here and on every connected peer (the web app), as a
     * root node carrying one `model` component.
     *
     * Pass the JobId when placing a generated model so the actor — and the `spawn`
     * every peer receives — records which job produced it. That is what makes
     * `FindSyncedActorByJobId` / the handle's `Find Spawned Actor` resolve later.
     * It rides *inside* the `model` component, because the hub keeps a known
     * component's extra keys but strips node-level fields it does not know.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    ALoomaSyncedActor* SpawnSyncedAsset(const FString& AssetId, const FString& Name, const FTransform& Transform,
        const FString& JobId = TEXT(""));

    /** Remove a synced actor here and on every connected peer. The hub cascades to its children. */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void DespawnSyncedActor(ALoomaSyncedActor* Actor);

    /** The actor for a node id, or null if this client does not have that node. */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    ALoomaSyncedActor* FindSyncedActor(const FString& NodeId) const;

    UFUNCTION(BlueprintPure, Category = "Looma")
    bool IsSyncConnected() const;

    // --- Which scene this client is on (outbound `openScene`) -----------------
    //
    // Which SCENE this client watches is a live, per-client choice made over the
    // socket, and changing it moves nobody else in the room. That is the asymmetry
    // worth holding on to: changing PERFORMANCE is still a reconnect, changing scene
    // is a message (docs/scene-format.md, "Choosing a scene (HAM-185)").

    /**
     * Subscribe this client to a saved scene: `{"type":"openScene","sceneId":...}`.
     * Console: `Looma.Scene <sceneId>`.
     *
     * Fire-and-forget, with no return value and nothing recorded as pending, because
     * the reply is a whole `scene` frame — the same frame that arrives on connect,
     * applied by the same HandleScene. There is nothing here for a caller to
     * correlate, and ActiveSceneId is deliberately not written optimistically: the hub
     * decides what we are on and HandleScene stays its single writer, so a refusal has
     * nothing to unwind.
     *
     * A refusal — an id the backend does not have, or a scene this client may not read
     * — comes back as a `sceneError` **frame and not a close**: the socket stays up and
     * stays usable, and we simply remain on the scene we were already on. That is the
     * opposite of the handshake's 4400/4403/4404 closes, which all mean start over.
     * HandleSceneError logs it.
     *
     * The id is not validated here. This client holds no catalogue to check it
     * against — a `scene` frame carries one id and its nodes, never its siblings — and
     * the hub is the authority on which ids exist and who may have them, so a local
     * guess could only ever disagree with it. Even an empty id goes as-is: the hub
     * answers that with the same `sceneError` as any other miss, so a second opinion
     * would buy one narrower message and one more thing to keep in step.
     *
     * **A send on a closed socket is dropped** — SendJson returns silently, and the
     * contract says the same ("a send before the socket is open is dropped",
     * docs/scene-format.md). Unlike a pose diff, which the next tick sends again, a
     * dropped `openScene` never happens again, so this one send says so rather than
     * inheriting that silence. A caller that can ask first should: `Looma.Scene` tests
     * IsSyncConnected() and prints its own line instead of reaching here.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void OpenScene(const FString& SceneId);

    /**
     * Open a scene named either way: `demo-costume-test` or `Costume Test`.
     * Console: `Looma.Scene <sceneId>` / `Looma.Scene "<name>"`.
     *
     * Resolved against `GET /scenes` first and only then sent, so **every** open costs
     * one metadata request — including one that names an id, which step 1 sent straight
     * through. That fast path was worth having and is not worth keeping. Sending
     * speculatively and falling back to the list only on a refusal was the alternative,
     * and what rules it out is what the send IS: an `openScene` is a real subscription
     * attempt with a real effect, not a probe. On a hit it subscribes — so the cheap
     * path is cheap exactly when it was not needed — and on a miss it costs two round
     * trips (the send, the refusal, then the fetch and a second send) where this costs
     * one HTTP call always. The miss is the case that matters, because it is the one a
     * person hunting for the right name is actually in.
     *
     * To be clear about what is NOT the objection, since the natural place to build it
     * would be right here: a refusal CAN be matched to the request that caused it.
     * `sceneError` echoes the requested `sceneId` straight back
     * (`_send_scene_error(ws, reason, message, scene_id)`, backend/app/sync.py), so
     * correlation is a comparison and not a guess about timing. It is simply not worth
     * what it costs — per-request state kept on a fire-and-forget send, to save one
     * small metadata GET at human typing speed.
     *
     * Uniform behaviour also means one failure message, said with the catalogue in hand
     * instead of a bare refusal from the hub.
     *
     * What step 1 actually protected is untouched: nothing fetches when a `scene` frame
     * arrives. This is still on demand, at typing speed, nowhere near the moment the
     * GLB downloads take every connection slot.
     *
     * MATCHING ORDER: exact id, then exact name, then name ignoring case. Ids win
     * because an id names exactly one scene and is what every other line here prints;
     * where a string is one scene's id and another's name, that is said out loud rather
     * than resolved silently. A name matching more than one scene is refused with the
     * ids that tell them apart — names are not unique and picking the first would be
     * choosing for the user, invisibly.
     *
     * If the list cannot be read at all, the argument is sent as an id and the hub
     * answers — which is exactly step 1's behaviour, kept for the case that made a fast
     * path attractive in the first place.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void OpenSceneByNameOrId(const FString& NameOrId);

    /**
     * Which saved scene the hub has us on. **Empty is an answer, not a failure**: it is
     * what an unsaved working scene reports, since that document has no row behind it
     * and the frame carries `sceneId: null`. It is also what we hold before the first
     * frame lands, and nothing in the string tells those two apart — which is why
     * LogActiveScene consults the socket and this getter cannot.
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString GetActiveSceneId() const;

    /**
     * Log the active scene's id and name. Console: `Looma.Scene` with no arguments.
     *
     * TWO LINES, deliberately. The id is local and immediate; the name costs a round
     * trip, because the `scene` frame carries `sceneId`, `performance`, `access`,
     * `version` and `nodes` and no name at all (`sync.scene_message`). Waiting to print
     * one resolved line would mean a backend that is down answers a question about
     * local state with silence, which is the wrong failure for a diagnostic — so the id
     * goes out at once and the name follows on its own line, naming the id it belongs
     * to so two quick invocations cannot be read crosswise.
     *
     * FETCHED ON DEMAND, never when a `scene` frame arrives. Scene arrival is the exact
     * instant every node starts pulling its GLB and all 16 of UE's per-host connection
     * slots are taken, so a request issued there queues behind the downloads and times
     * out against a backend answering in a tenth of a second — the same reason
     * Connect's OnConnected carries no auth probe.
     *
     * NOTHING IS CACHED. A rename through `PUT /scenes/{id}` puts nothing on the wire,
     * so no inbound message could invalidate a remembered name and the only honest rule
     * would be "invalidate always", which is not a cache. A console diagnostic that
     * confidently prints a stale name is worse than one that costs a small metadata GET
     * at human typing speed.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void LogActiveScene();

    /**
     * Log every scene this identity may open, id and name, marking the active one.
     * Console: `Looma.Scenes`.
     *
     * The answer to the question `Looma.Scene <name-or-id>` provokes — "and what are
     * the ids?" — from the client that accepts them, rather than from a browser tab.
     * It is FetchScenes with the result printed instead of searched, which is the shape
     * that helper was extracted into.
     *
     * UNCAPPED, unlike the "no such scene" message, which names twenty and counts the
     * rest. The cap is right there and wrong here: that message is a side effect of a
     * different request and has to stay a sentence, while this one IS the request, and
     * truncating the answer to a question somebody explicitly asked is the one place
     * truncation cannot be defended. There is no pagination on the route either, so a
     * client-side cap could only hide rows the backend was willing to show; if the list
     * ever outgrows a console, the answer is a filter argument, not a shorter answer.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void LogScenes();

    // --- Which performance this client is in ----------------------------------
    //
    // The scene is chosen inside a workspace; the workspace is resolved once, at
    // handshake time, from the `hello`. So this pair is read-only where the scene pair
    // is not: there is no message that moves a socket already up, which is why
    // switching performance is a reconnect and switching scene is not.

    /**
     * The workspace this socket is in, as of the last `scene` frame. `Id` is empty
     * until one arrives.
     *
     * A whole-object read rather than per-field getters: a caller that wants to REPORT
     * the performance needs all of it, and a caller that wants to ROUTE on it wants
     * GetActivePerformanceId() below. Nothing wants one descriptive field on its own,
     * and `Name` handed out alone would be unreadable — see FLoomaPerformance.
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FLoomaPerformance GetActivePerformance() const;

    /**
     * Just the id: the routing key, and the one field that is true even when the row
     * behind it has gone. Kept beside the whole-object getter, and parallel to
     * GetActiveSceneId(), because it is what every non-display caller actually holds —
     * a REST path to build, or an id to compare a request's answer against.
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString GetActivePerformanceId() const;

    /**
     * Ask the hub to put this client in a different performance, and reconnect —
     * because a switch **is** a reconnect. A socket's performance is chosen exactly
     * once, from `hello.performanceId`, and never changes for the life of that
     * connection (docs/scene-format.md, "Performance selection (HAM-197)"), so there is
     * no message to send and nothing here could avoid dropping the socket.
     * Console: `Looma.Performance <performance-id>`.
     *
     * A REQUEST, never a grant, and deliberately not validated here — the same posture
     * as GuestDisplayName. The hub resolves identity first and then runs the identical
     * non-enumerating `get_visible` check `GET /performances/{id}` runs, so an id this
     * client thought fine can still be refused, and an id it thought malformed is not
     * its business: `sync.py` does not have a "malformed performance id" rejection at
     * all, because a garbage string simply fails that same lookup. The empty check
     * below is not validation either — it is declining to send a field whose only
     * possible outcome is a refusal.
     *
     * The id OUTLIVES the socket, on purpose: it is the user's stated intent, so a
     * later `Looma.Reconnect`, a backend-address change and every automatic retry all
     * keep the room they chose. Only another call to this moves it.
     *
     * Refusal arrives as a bare close — 4403 or 4404, no error frame — which stops the
     * retry loop for good rather than being waited out. See OnClosed.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void SwitchPerformance(const FString& PerformanceId);

    /**
     * Switch performance and, once the new socket is actually in one, open the cue at
     * `CueIndex`. Console: `Looma.Performance <performance-id> <index>`.
     *
     * The two halves cannot simply be called in order, and that is the whole of this
     * function. A switch is a RECONNECT, so the socket the `openScene` needs does not
     * exist when this returns; a send before it opens is dropped silently; and the
     * running order has to be read for the performance we LAND in, which the hub is
     * free to make a different one from the one we asked for. So the index is parked
     * and fired from ConfirmRequestedPerformance — the one moment "the switch landed,
     * and here is where we actually are" first becomes true.
     *
     * NOT VALIDATED AGAINST THE TARGET'S RAIL FIRST, though `GET /performances/{id}/cues`
     * would answer before the socket is dropped. Three reasons, and the last is
     * decisive. It would re-ask a question the handshake asks anyway, so a scene shared
     * or archived in between makes the two disagree and this one refuses a switch the
     * hub would have allowed. It would read the rail of the performance we ASKED for,
     * which is exactly the list that may not be the one we end up in — the bug the
     * fire-on-confirmation rule exists to avoid. And every performance on this backend
     * currently has an empty running order, so a pre-flight check would refuse every
     * two-argument command outright rather than switching and reporting an empty rail,
     * which is a worse answer to the state the user is actually in.
     *
     * The cost of that choice is real and is not hidden: an index that turns out to be
     * out of range is discovered AFTER a reconnect everyone in the room has already
     * paid for. The failure line therefore separates the halves — the switch stands,
     * only the jump was dropped — because "nothing happened" would be a lie and
     * "it failed" would be two.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void SwitchPerformanceToCue(const FString& PerformanceId, int32 CueIndex);

    /**
     * The performance id this socket asked for and the hub has not yet confirmed;
     * empty when nothing is in flight.
     *
     * Exists because GetActivePerformance() is stale for the whole of that window and
     * does not look it. Nothing clears the confirmed value on a disconnect — see
     * HandleScene — so between a switch starting and the new socket's first `scene`
     * frame, the "current" performance is the room we are LEAVING, reported with a
     * perfectly valid-looking id. Anything that shows one of these must show the other
     * while they can disagree.
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString GetPendingPerformanceId() const;

    /**
     * Log every performance this identity may see — id, name and our own role in it —
     * marking where this socket is. Console: `Looma.Performances`.
     *
     * `role` is printed because it is the field that predicts the next refusal: it is
     * the difference between a workspace this caller may reshape and one it may only
     * watch, and it is per-caller, so no other client's answer is ours.
     *
     * Honest during a switch, which is the one moment the marker could lie. The
     * confirmed performance is the room being LEFT until the new socket's `scene` frame
     * lands (nothing clears it on a drop), so while GetPendingPerformanceId is set the
     * two are marked differently and a preamble says which is which.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void LogPerformances();

    // --- The running order (cues) ---------------------------------------------
    //
    // Moving along the line is an `openScene` and nothing more: a cue names a scene in
    // the performance this socket is ALREADY in, so there is no reconnect here. That is
    // the whole difference from switching performance, and the reason these two live in
    // different steps.

    /**
     * Open the scene at a position on the current performance's running order.
     * Console: `Looma.Cue <index>`.
     *
     * **0-based, and the index is a POSITION IN THE LIST, not an `order_idx`.** The
     * route sorts by `order_idx, rowid` but nothing makes that column dense or
     * zero-based — `add_cue` takes an optional `order_idx` and `reorder_cues` rewrites
     * them by list position — so matching the argument against the column would work
     * perfectly on freshly-reordered data and mis-fire on anything hand-inserted.
     *
     * Out of range reports the range rather than clamping. Clamping a cue number is how
     * a performance ends up showing the wrong scene to a room full of people, and the
     * off-by-one it hides is exactly the thing a console is being used to find.
     *
     * REFUSED WHILE A PERFORMANCE SWITCH IS IN FLIGHT. The running order we could read
     * belongs to the room being left, and the socket that would receive the `openScene`
     * may be in a different one by the time the answer lands — so firing it is a race
     * whose two halves each look correct. LogCues is allowed in that window and this is
     * not, which is the whole distinction: reading a stale list you have been TOLD is
     * stale is fine, acting on one is not.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void OpenCue(int32 CueIndex);

    /**
     * Log the current performance's running order — index, scene id and label — marking
     * the cues whose scene is the active one. Console: `Looma.Cue` with no arguments.
     *
     * CUES, PLURAL, MAY BE MARKED. A scene can be on the line more than once (a
     * reprise), and the wire moves by scene id rather than by cue position — `openScene`
     * carries a `sceneId` and the `scene` frame answers with one — so this client
     * genuinely does not know which of two identical cues is live. Marking both and
     * saying so is the honest answer; marking the first would be a guess dressed as a
     * fact.
     *
     * Scene NAMES are deliberately not fetched. `FetchScenes` has them and a second
     * request would pair them up, but it would double the failure surface of a
     * read-only command for a cosmetic field and need a policy for "the cues came back
     * and the scenes did not" — a half-rendered listing. The scene id is the string
     * every other command here accepts, so this hands back something directly usable,
     * and `Looma.Scenes` pairs ids with names one command away.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void LogCues();

    // --- Local selection (outbound `selection`) -------------------------------
    //
    // What this client has selected, reported to the hub so every other client can
    // draw a border on those nodes in our colour. The normative contract is
    // docs/scene-format.md, "What each client has selected — the `selection` message".
    //
    // Three properties of that contract shape everything below. The message carries
    // the **whole** selection and never a delta, so the canonical call is a whole-set
    // replace and the others are conveniences over it. An **empty set is a real
    // message** — `[]` is how a border is cleared, and there is no teardown message —
    // so nothing here may treat "nothing selected" as "nothing to say". And the hub
    // stamps `clientId` and `color` itself, discarding anything we put there, so we
    // send `ids` and nothing else.
    //
    // This is presence, not scene state: it never enters the document, is never saved,
    // and dies with the socket.

    /**
     * Replace the whole local selection. The canonical call — the wire carries a whole
     * set, so this mirrors it exactly, and SelectNode / DeselectNode / ClearSelection
     * are conveniences that read-modify-write through here.
     *
     * Null entries and duplicates are dropped. Actors are held weakly, so one
     * destroyed while selected leaves the selection by itself.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Selection")
    void SetLocalSelection(const TArray<ALoomaSyncedActor*>& Actors);

    /** Add one node to the local selection. No-op if it is already in it. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Selection")
    void SelectNode(ALoomaSyncedActor* Actor);

    /** Remove one node from the local selection. No-op if it was not in it. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Selection")
    void DeselectNode(ALoomaSyncedActor* Actor);

    /**
     * Select nothing. Not a quiet local reset: it sends `{"ids": []}`, which is what
     * clears our borders in every other client.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Selection")
    void ClearSelection();

    /** The actors currently selected locally. Destroyed ones are already gone from it. */
    UFUNCTION(BlueprintPure, Category = "Looma|Selection")
    TArray<ALoomaSyncedActor*> GetLocalSelection() const;

    /** Whether this node is in the local selection. */
    UFUNCTION(BlueprintPure, Category = "Looma|Selection")
    bool IsNodeSelected(ALoomaSyncedActor* Actor) const;

    /** The local selection as node ids, sorted — what the next `selection` will carry. */
    UFUNCTION(BlueprintPure, Category = "Looma|Selection")
    TArray<FString> GetLocalSelectionIds() const;

    /** Our selection changed and has been reported. See FLoomaSelectionEvent. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Selection")
    FLoomaSelectionEvent OnLocalSelectionChanged;

    // --- Presence: who else is in the room (inbound `clients`) ----------------
    //
    // The normative contract is docs/scene-format.md, "Who else is in the room — the
    // `clients` message". The hub gives every connected client a colour and tells
    // everyone who is present, so a shared scene reads as shared.
    //
    // Three properties of that contract shape everything below. The roster is the
    // **whole room every time**, so it is replaced wholesale and never merged: a
    // client that has left is simply absent, and dropping it is the only thing that
    // clears its borders, because there is no teardown message. Its **order is join
    // order**, which step 2 uses as the tiebreak when two clients claim one node, so
    // nothing here sorts it. And **nothing in it is ours to invent** — the colour is
    // server-assigned, the display name server-resolved, `kind` whatever the hub said.
    //
    // This is presence, not scene state: never merged into the document, never saved,
    // never sent in a `scene`, and cleared the moment the socket dies.

    /**
     * The **other** clients in the room, in roster order. Empty while disconnected.
     *
     * Our own entry is deliberately not in this list — GetOwnClientId() and
     * GetClient() reach it instead. That is not tidiness: everything downstream of
     * this list draws a border in that client's colour, and our own colour drawn on
     * our own selection is exactly the confusion per-client colours exist to remove.
     * Our selection already has its local highlight.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    TArray<FLoomaClient> GetClients() const;

    /**
     * Our own id in the room — the roster's `you` — or empty until a roster lands.
     *
     * Worth more than it looks: it is the only way this client can learn its own
     * room name. `GET /auth/me` mints a fresh random `Guest-xxxxxx` on every call
     * when no session is held, so that name matches nothing anybody else sees; the
     * roster's self entry is the one place our real name appears. Feed this to
     * GetClient() to read it.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    FString GetOwnClientId() const;

    /**
     * One client by id, ours included — the self entry is held separately from the
     * remote list but is still findable here, so `GetClient(GetOwnClientId())` is how
     * you read the colour and name everyone else sees for us.
     *
     * Note the self entry's `Selection` is the hub's copy, refreshed only on a join
     * or a leave, so it is stale for us the moment we click something. A reader that
     * wants our live selection wants GetLocalSelectionIds().
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    bool GetClient(const FString& ClientId, FLoomaClient& OutClient) const;

    /**
     * The room moved. See FLoomaClientsEvent.
     *
     * Also fires for an inbound `selection`, which changes no membership at all: a
     * client's selection is part of FLoomaClient and part of its equality, so "who is
     * here" and "what are they working on" are one question with one event. A second
     * delegate for selections would fire in lockstep with this one on every roster —
     * every roster carries selections — and give a consumer two ways to be told the
     * same thing.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Presence")
    FLoomaClientsEvent OnClientsChanged;

    // --- The claim ledger: who gets to draw a border on a node ----------------
    //
    // Nothing stops two people selecting one node and **nothing is locked** — both can
    // still edit it. This is a drawing rule only, so that one node has one border, and
    // it is the contract's "Two clients on one node — first claim wins".
    //
    // The ledger is nodeId -> claimants, oldest claim first, maintained by the four
    // rules the spec gives: append a sender for each id it did not previously hold,
    // remove it from each id it no longer sends, remove a departed client from every
    // list, and draw the head. Every client receives hub messages in the same order,
    // so maintaining arrival order locally is what makes every client agree on the
    // winner with no arbitration and no extra protocol. A real per-selection lock is
    // planned and will replace this rule, which is why the tiebreak lives here rather
    // than being re-decided by whatever draws.

    /**
     * Who currently wins the border on this node — the oldest live claim — or false if
     * nobody claims it. Ours is never in the ledger, so this only ever names someone
     * else.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    bool GetNodeBorderOwner(const FString& NodeId, FLoomaClient& OutClient) const;

    /**
     * Everyone claiming this node, oldest claim first — the head is the winner. For
     * a UI that wants to say "and two others", which the border alone cannot.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    TArray<FString> GetNodeClaimants(const FString& NodeId) const;

    /**
     * The nodes this client both selected **and won**, in that client's selection
     * order — one outline group's worth, which is what makes it the call a renderer
     * wants rather than GetClient().Selection.
     *
     * Ids for nodes this client does not hold are included, deliberately: a selection
     * legitimately races the spawn that created its node, filtering here would drop a
     * claim nothing ever re-sends, and resolving an id to an actor is the caller's job
     * anyway (FindSyncedActor). Draw nothing for an id you cannot resolve; do not
     * treat it as an error.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    TArray<FString> GetClientBorderNodes(const FString& ClientId) const;

    // --- Drawing the borders (custom depth stencil) ---------------------------
    //
    // The plugin does not draw. It marks: it sets `CustomDepthStencilValue` on the
    // primitives whose borders this client should show, and publishes the per-client
    // colours, leaving a consumer project's post-process material to turn the two into
    // an outline. That split is forced — `"CanContainContent": false` means the plugin
    // can ship no material and no parameter collection — and it is also the right
    // shape: a project that already has an outline material wires this in without
    // adopting a second one. The README's *Wiring the outline* is the whole recipe.
    //
    // The weighting mirrors the web client (frontend/src/scene/Scene.jsx): thick on
    // the claimed node, thinner on everything under it, in the owner's colour. Two
    // subtractions come before it, both from `remoteBorders` in presence.js. Our own
    // selection is removed from remote borders first, own and child alike — on our
    // screen ours always wins, so a remote claim can never make us lose track of what
    // we hold. And a node someone claimed outright never doubles as another client's
    // descendant hint, the same precedence a selection has over a child hint locally,
    // so one node keeps one border however many claimed subtrees it sits under.
    //
    // This plugin does NOT drive a stencil for the local selection. That is HAM-188's
    // half and a consumer's own business — its visuals never change when the room
    // does, which is the point.

    /**
     * What to draw, one entry per remote client that holds a border, in roster order.
     *
     * For a consumer driving its own material instead of the parameter collection.
     * Recomputed when the room, the local selection or the scene moves — bind
     * `On Clients Changed` and re-read, or just read it each frame; it is a cached
     * array, not a computation.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    TArray<FLoomaBorderGroup> GetRemoteBorderGroups() const;

    /**
     * Clients holding a border that there was no stencil slot left to draw.
     *
     * Never empty silently: a room bigger than LoomaRemoteBorderSlots is a real state,
     * and a client that is present, working and invisible is indistinguishable from an
     * empty room unless something says so. Show these in a HUD, the way the web names
     * them in its room panel; the plugin also logs the list once each time it changes.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Presence")
    TArray<FString> GetUndrawnClients() const;

    /**
     * Log the room one line per client — id, name, kind, role, colour and selection —
     * with our own entry marked. Console: `Looma.Room`.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Presence")
    void LogRoom() const;

    /**
     * Log the claim ledger both ways round: each claimed node with its claimants
     * oldest-first, and each client with how much of its selection it actually wins.
     * The two differ whenever anyone is contested, and that difference is the whole
     * point of the tiebreak. Console: `Looma.Claims`.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Presence")
    void LogClaims() const;

    // --- Connection control / diagnostics -------------------------------------

    /**
     * Drop the socket (if any) and connect again straight away, re-reading
     * Project Settings > Plugins > Looma Scene Sync — so this is also how a changed
     * backend address takes effect. Console: `Looma.Reconnect`.
     *
     * Scene state is kept: the hub answers a fresh connection with the whole `scene`
     * document, which reconciles what we hold and drops what it no longer has.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void Reconnect();

    /** The hub URL this client uses: the REST base, ws(s) scheme, `/ws/scene`. */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString GetSceneSyncUrl() const;

    /**
     * One line: state, hub URL, REST base, and what we are holding. Cheap — it reports
     * the socket, it does not talk to the backend. Console: `Looma.Status`.
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString GetConnectionStatusText() const;

    /**
     * Log `GetConnectionStatusText()`, then `GET /health` and log whether the backend
     * answered — which separates "the socket is down" from "nothing is listening".
     * Console: `Looma.Status`.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void LogConnectionStatus();

    // --- Auth discovery ------------------------------------------------------
    // Probed on Initialize and whenever the backend address moves — never only on
    // demand, because a UI has to be able to ask this before a human has typed
    // anything — and re-probed on a lengthening backoff for as long as the answer is
    // still Unknown. `Unknown` is a transient state, not a resting place: see
    // ScheduleHealthRetry for the connection-pool starvation that makes a single
    // attempt unreliable over a real network.

    /** What `GET /health` last said about auth. See ELoomaAuthState. */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    ELoomaAuthState GetAuthState() const;

    /**
     * We hold a `/health` answer we can act on. Gate every "show the login screen"
     * decision on this first: false means undecided, never "no login needed".
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    bool IsAuthStateKnown() const;

    /** The backend requires a login. False while the state is still Unknown. */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    bool IsAuthEnabled() const;

    /** The backend accepts self-registration. Only ever true when auth is enabled. */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    bool IsRegistrationEnabled() const;

    /**
     * The auth state became known, or moved — a different backend, or one that was
     * reconfigured. Fires only on an actual change, so binding is enough and nothing
     * needs to poll; a change *to* Unknown is a change worth hearing about, since it
     * is what invalidates a login screen drawn from the previous backend's answer.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Auth")
    FLoomaAuthStateEvent OnAuthStateChanged;

    // --- Auth: the session ---------------------------------------------------
    // "Who am I", as against the block above's "what does this backend require".
    //
    // The token lives in memory for the lifetime of the game instance and nowhere
    // else — no config, no save game, no log line. A fresh editor session is a fresh
    // login.

    /**
     * Who the backend last told us we are. `Kind == Unknown` until something
     * establishes it, and Unknown is not a synonym for guest: this client cannot name
     * its own guest identity, because the hub derives `Guest-xxxxxx` from our WS
     * clientId and only it knows the result.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    FLoomaIdentity GetIdentity() const;

    /**
     * We hold a session token. A different question from "is my identity a user":
     * a token can be revoked or expire server-side while we still hold the bytes, and
     * only the backend can say. Kept separate so neither can be mistaken for the
     * other.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    bool HasAuthToken() const;

    /** One line naming the identity and whether a token is held. Never the token itself. */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    FString GetIdentityText() const;

    /**
     * We restored a session from disk and `GET /auth/me` has not confirmed it yet.
     *
     * True only in that window, which lasts from Initialize until the health probe
     * lands and the validation it triggers answers — seconds, on a contended
     * connection. In it, GetIdentity() reports the display name that was persisted
     * alongside the token and `Kind == Unknown`, because a name off the disk is a
     * label, not evidence: only the backend can say whether the token is still live,
     * and only `Kind` records that it has. Show the name if you like — that is why it
     * is there, and why a launch does not have to look anonymous — but gate anything
     * that depends on *being* that account on `Kind == User`, never on the name being
     * non-empty.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth")
    bool IsIdentityProvisional() const;

    /** Who we are changed. See FLoomaIdentityEvent for why this is not OnAuthStateChanged. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Auth")
    FLoomaIdentityEvent OnIdentityChanged;

    /**
     * Forget the session, then ask the backend to revoke it — in that order, and the
     * local half never depends on the answer. Console: `Looma.Logout`.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Auth")
    void Logout();

    /**
     * `GET /auth/me` — re-ask the backend who we are, and adopt the answer.
     *
     * That route never answers 401: an unauthenticated caller gets a *guest* identity
     * with a 200 (backend/app/auth/routes.py), so a token we hold that comes back as
     * a guest is a token the backend no longer honours. Console: `Looma.Whoami`.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Auth")
    void RefreshIdentity();

    /**
     * `POST /auth/login`. On success the session is adopted before OnComplete runs,
     * so a caller can read GetIdentity()/HasAuthToken() straight away. On failure
     * nothing else moves: the socket stays up, the scene stays loaded, and any
     * identity we already had is still ours — an auth-enabled backend has to stay
     * usable by someone who cannot or will not log in.
     *
     * Not a UFUNCTION: Blueprint gets `Login` (ULoomaLoginAction), which is this call
     * with exec pins. The request lives here because the subsystem owns the token and
     * because `Looma.Login` needs the same call — one implementation, two front doors.
     */
    void RequestLogin(const FString& Username, const FString& Password,
        TFunction<void(bool bSuccess, const FLoomaIdentity& Identity, const FString& Error)> OnComplete);

    /**
     * Put `Authorization: Bearer <token>` on a request, if we hold a token. A request
     * without one is a guest request, not an error, so this is always safe to call —
     * every backend route serves guests.
     *
     * The single place that header is attached, and public for that reason: the two
     * async actions are separate UCLASSes and need it. It takes the request rather
     * than returning the string on purpose, and that is exactly the property that
     * makes exposing it safe — no caller ever holds a copy of the token, and a
     * `GetToken()` getter would be one careless log line away from a shared file.
     *
     * **Call this after SetURL.** Being the only attacher lets it also enforce that
     * the token goes nowhere but the configured backend, and it needs the URL to do
     * that. A caller that gets the order wrong simply gets no header, which is the
     * safe failure.
     */
    void ApplyAuthHeader(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request) const;

    /**
     * The same header as a WebSocket upgrade header, for
     * FWebSocketsModule::CreateWebSocket's third parameter. An overload rather than a
     * shared `FString MakeBearer()` so that still nothing anywhere returns the token;
     * the header's *spelling* is single-sourced in the .cpp instead.
     */
    void ApplyAuthHeader(TMap<FString, FString>& UpgradeHeaders) const;

    /**
     * Put `X-Client-Id: <our clientId>` on an **asset-creating** request, so the work
     * a guest does over one-shot HTTP is credited to the same `Guest-xxxxxx` the room
     * roster already shows them under. Without it the backend has nothing to key a
     * guest name off for a plain POST, and mints a fresh random one per request
     * (HAM-176).
     *
     * A NAME SEED, and nothing else. It proves nothing and authenticates nothing: both
     * providers resolve a real session first and an authenticated one always wins over
     * this, so it can never be used to claim `kind: "user"` or anyone else's session
     * (see CLIENT_ID_HEADER in backend/app/auth/provider.py). Never reach for it where
     * an identity actually has to be established — that is ApplyAuthHeader's job.
     *
     * Scoped to asset-creating requests as the backend documents, rather than attached
     * blanket-fashion to every REST call: that is the only place the value means
     * anything, and a header travelling further than its purpose invites being
     * mistaken for one that matters.
     */
    void ApplyClientIdHeader(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request) const;

    // --- Generation jobs (observe) -------------------------------------------

    /** Any change to a job (state / progress / queue position / enhanced prompt). */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaGenerationJobEvent OnGenerationJobUpdated;

    /** Candidate images are ready to pick (State == AwaitingImage). */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaGenerationJobEvent OnGenerationImagesReady;

    /** The model is ready (State == Done); AssetId / AssetUrl are populated. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaGenerationJobEvent OnGenerationJobDone;

    /** The job failed (State == Failed); Error is set. Despawn the orb here. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaGenerationJobEvent OnGenerationJobFailed;

    /** The scene-sync socket connected (after the hello handshake). */
    UPROPERTY(BlueprintAssignable, Category = "Looma")
    FLoomaSyncConnectionEvent OnSyncConnected;

    /** The scene-sync socket dropped (a reconnect is scheduled). */
    UPROPERTY(BlueprintAssignable, Category = "Looma")
    FLoomaSyncConnectionEvent OnSyncDisconnected;

    /**
     * Per-job event handle — bind these instead of the OnGeneration* events above
     * and you only hear about this one job, with no JobId filtering and no burst
     * from the connect-time queue hydrate.
     *
     * Creates the handle on first request and returns the same one thereafter.
     * A handle for a job that is already known replays its current state on the
     * next tick, so binding straight after this call cannot miss anything.
     * `Submit Generation` hands you one directly; use this to attach to a job you
     * found some other way (GetAllGenerationJobs, or another client's job).
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    ULoomaGenerationHandle* GetGenerationHandle(const FString& JobId);

    /**
     * Record the pose a job was submitted with, so every reader of that job — the
     * cache, `GetGenerationJob`, the hub-wide events and any handle — reports it.
     *
     * Needed because the backend announces a job during `submit()`, before it has
     * stored the pose, so the first `generation` event carries none. A job's
     * suggested pose is set once and never cleared, so once learned (from here or
     * from any event) it is merged into every later snapshot.
     *
     * Not exposed to Blueprint: `Submit Generation` calls this for you.
     */
    void NoteSuggestedTransform(const FString& JobId, const FTransform& SuggestedTransform);

    /** Look up a cached job by id. Returns false if unknown. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    bool GetGenerationJob(const FString& JobId, FLoomaGenerationJob& OutJob) const;

    /** Every job currently known to this client. */
    UFUNCTION(BlueprintPure, Category = "Looma|Generation")
    TArray<FLoomaGenerationJob> GetAllGenerationJobs() const;

    /** Find the spawned actor for a generate-at-location job, if it has spawned. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    ALoomaSyncedActor* FindSyncedActorByJobId(const FString& JobId) const;

    // --- Generation jobs (drive) ---------------------------------------------
    // SubmitGeneration is the async node ULoomaSubmitGenerationAction. The calls
    // below are fire-and-forget; the resulting state arrives via the events above.

    /** Pick a candidate image (starts 3D reconstruction). POST /generate/{id}/select. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void SelectImage(const FString& JobId, const FString& ImageId);

    /** Re-roll candidate images. NImages <= 0 keeps the server default. POST .../regenerate. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void RegenerateImages(const FString& JobId, const FString& Prompt, int32 NImages = 0);

    /** Cancel a job. DELETE /generate/{id}. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void CancelGeneration(const FString& JobId);

    // --- Backend URLs --------------------------------------------------------

    /**
     * REST/asset base for direct (non-proxied) access, e.g. "http://127.0.0.1:8000" —
     * the settings' Backend URL, normalised (scheme filled in, no trailing slash).
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString GetRestBase() const;

    /**
     * Turn a backend-relative path into an absolute URL a native client can hit.
     * Passes absolute (http...) URLs through unchanged; strips a leading "/api"
     * (the web proxy prefix) so "/api/static/x.png" -> "http://<host>/static/x.png".
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString ResolveBackendUrl(const FString& PathOrUrl) const;

    // Every knob lives on ULoomaSceneSyncSettings — Project Settings > Plugins >
    // Looma Scene Sync — and is read live from there, so an edit needs no restart.

private:
    void Connect();
    /** Clear the handlers and close the socket, so a dead one cannot fire a retry. */
    void CloseSocket();
    /** A settings edit landed: reconnect if the backend address moved. */
    void OnSettingsChanged();

    /**
     * `GET /health` — the one implementation behind both callers. bLogDiagnostics is
     * what makes it the `Looma.Status` diagnostic: the reachability triage naming
     * likely causes. The unprompted probes (startup, settings change, retry) pass
     * false, because that prose on every editor launch — where the backend routinely
     * is not up yet, and the socket layer already says so once per retry — is noise
     * nobody asked for. The auth state is refreshed either way; that is the half that
     * is not a diagnostic.
     *
     * The flag separates two more things than the logging. A quiet probe gets a
     * background timeout and arms a retry; a diagnostic gets a short timeout and never
     * loops, because someone is watching and wants one answer now.
     */
    void ProbeHealth(bool bLogDiagnostics);

    /**
     * Arm the next quiet re-probe, lengthening the wait each time (exponential, capped).
     * No-op once the auth state is known — that is the "only while Unknown" half of the
     * rule, enforced here so no caller has to remember it.
     *
     * Armed when a probe is *sent*, not when one fails, so that "the state is Unknown
     * and nothing is pending" cannot be represented: a request the HTTP layer drops
     * without ever completing cannot strand the state machine.
     */
    void ScheduleHealthRetry();

    /**
     * The question is settled, or the address moved: stop asking and forget the
     * backoff. Also releases the in-flight guard, deliberately abandoning any probe
     * still outstanding — its answer would be discarded on arrival anyway (it names a
     * backend we have moved off, or a state we have since learned).
     */
    void CancelHealthRetry();

    /** Count down the re-probe. Called from Tick ahead of the reconnect early-out. */
    void TickHealthRetry(float DeltaTime);

    /** Adopt an auth state, broadcasting OnAuthStateChanged only if it actually moved. */
    void SetAuthState(ELoomaAuthState NewState);

    /**
     * Take on a session: the token and the identity it belongs to, which only mean
     * anything together. Persists it and re-handshakes the socket.
     */
    void AdoptSession(const FString& Token, const FLoomaIdentity& NewIdentity);

    /**
     * The in-memory half of a session change, with no persistence and no reconnect.
     * Exists so AdoptSession, ClearSession and LoadSavedSession share one definition
     * of "the session is now this" — the restore path in particular must not write
     * back the file it just read, nor reconnect a socket that does not exist yet.
     */
    void SetSession(const FString& Token, const FLoomaIdentity& NewIdentity);

    /**
     * Where the session is kept: `<Project>/Saved/LoomaSceneSync/Session.json`.
     *
     * Under `Saved/` and emphatically **not** in Project Settings. A
     * `UPROPERTY(Config)` on ULoomaSceneSyncSettings — the obvious-looking answer,
     * since that is exactly where BackendUrl lives — would write to
     * `Config/DefaultGame.ini`, which every consumer project has in git. A session
     * token must not be committable, so nothing here goes near GConfig.
     */
    FString GetSessionFilePath() const;

    /** Write the current session out. Best-effort: a failure leaves the live session working. */
    void SaveSession() const;

    /** Remove the persisted session. Paired with ClearSession, so logout/expiry/backend-change all cover it. */
    void DeleteSavedSession() const;

    /**
     * Restore a session from disk, if there is one for *this* backend. Called from
     * Initialize before Connect, so the first handshake already carries the bearer —
     * connecting as a guest and then reconnecting would show every other client in the
     * room a join/leave flicker on every launch, for nothing.
     */
    void LoadSavedSession();

    /**
     * Drop the token and the identity. Cannot fail and never asks the backend, which
     * is what lets every caller treat forgetting a session as unconditional.
     *
     * bRehandshake re-opens the socket so the hub re-resolves us as a guest — which is
     * what every caller wants except OnSettingsChanged, which is already reconnecting
     * for the address change and would otherwise connect twice, the second tearing
     * down the socket the first had just made.
     */
    void ClearSession(bool bRehandshake = true);

    /**
     * Put the session token into an outbound `hello` as its `token` field — the wire's
     * documented last-resort transport, after the bearer header and the session cookie
     * (docs/scene-format.md). No-op when we hold none.
     *
     * The only place a token is written into a *message body* rather than a header, and
     * therefore the reason SendJson must never log what it serialises. Named apart from
     * ApplyAuthHeader so that distinction is visible at the call site: an overload would
     * have made "header" mean two different things.
     */
    void ApplyAuthToken(const TSharedRef<FJsonObject>& Hello) const;

    /** Store an identity, broadcasting OnIdentityChanged only if it actually moved. */
    void SetIdentity(const FLoomaIdentity& NewIdentity);

    void SendJson(const TSharedRef<FJsonObject>& Msg);

    // Inbound — one handler per wire message (see the class comment).
    void OnRawMessage(const FString& Text);
    /** The whole document: reconcile against what we hold, dropping nodes it no longer has. */
    void HandleScene(const TSharedPtr<FJsonObject>& Msg);
    void HandleSpawn(const TSharedPtr<FJsonObject>& Msg);
    void HandleDespawn(const TSharedPtr<FJsonObject>& Msg);
    void HandleTransform(const TSharedPtr<FJsonObject>& Msg);
    void HandleReparent(const TSharedPtr<FJsonObject>& Msg);
    void HandlePatch(const TSharedPtr<FJsonObject>& Msg);
    void HandleGeneration(const TSharedPtr<FJsonObject>& Msg);
    /**
     * A refused `openScene` or a refused edit, sent to this client alone. A frame and
     * not a close, so there is nothing to tear down here.
     */
    void HandleSceneError(const TSharedPtr<FJsonObject>& Msg);

    /** The room roster: rebuild RemoteClients / SelfClient wholesale. See the Presence block. */
    void HandleClients(const TSharedPtr<FJsonObject>& Msg);
    /** One client's whole selection, replacing what it held. See the claim-ledger block. */
    void HandleSelection(const TSharedPtr<FJsonObject>& Msg);

    // --- Claim ledger maintenance --------------------------------------------
    // The spec's four rules, one function each, so that nothing else in the plugin
    // decides who draws what. See the public claim-ledger block.

    /** Rule 1: append a claimant to a node's list, if it is not already on it. */
    void ClaimNode(const FString& NodeId, const FString& ClaimantId);
    /** Rule 2: drop a claimant from a node's list, and the list itself once empty. */
    void ReleaseNode(const FString& NodeId, const FString& ClaimantId);
    /** Rule 3: a departed client holds nothing. The one operation that scans the ledger. */
    void ReleaseAllClaims(const FString& ClaimantId);
    /**
     * Move one client from the set it held to the set it now sends: append for what is
     * new, release what it let go, and leave everything it still holds exactly where
     * it is. That last clause is the tiebreak — re-appending an unchanged claim would
     * silently promote a client to the back of a queue it was already at the front of.
     */
    void MoveClaims(const FString& ClaimantId, const TArray<FString>& Held, const TArray<FString>& Next);

    // --- Border rendering ------------------------------------------------------

    /**
     * The room, the local selection or the scene moved, so the borders need working
     * out again. A flag rather than an immediate recompute: a roster arriving while
     * four nodes spawn is one redraw, not five, and the two events that dirty this
     * most often — a `selection` and a local selection change — can both land in the
     * same frame.
     */
    void MarkBordersDirty() { bBordersDirty = true; }

    /** Recompute BorderGroups and push them to the stencil and the collection, if dirty. */
    void TickBorders();

    /** Work out who draws what, apply it, and name anyone the budget left out. */
    void RefreshRemoteBorders();

    /** Write one group's stencil value onto a node's primitives; slot 0 clears. */
    void ApplyStencilToNode(const FString& NodeId, int32 StencilValue, TSet<TWeakObjectPtr<UPrimitiveComponent>>& OutTouched);

    /** Publish the slot colours to the configured Material Parameter Collection. */
    void PublishBorderColors();

    /**
     * Forget the room, because presence dies with the socket.
     *
     * Called from every one of the three ways a socket ends — CloseSocket for a
     * deliberate teardown, and the OnClosed / OnConnectionError handlers for a drop,
     * which do NOT route through CloseSocket. Missing the last two is the bug worth
     * naming: the roster would then survive the disconnection that invalidated it, and
     * step 3 would leave a departed client's borders on screen until a reconnect that
     * may never come. Broadcasts OnClientsChanged if the room was not already empty,
     * which is what takes them down.
     */
    void ClearPresence();

    /**
     * `GET /scenes` once, handing the rows to OnDone — with bOk false and an empty array
     * if the list could not be read, having already said why.
     *
     * The single door to that route. Two callers today ask opposite questions of the
     * same answer — LogActiveScene resolves an id to a name, OpenSceneByNameOrId
     * resolves a name to an id — and a listing command would be a third that only
     * prints it. Keeping the request, its headers, its timeout and the reasoning behind
     * the route choice in one place is what stops those diverging.
     *
     * Private, with the console reaching it through Log-style methods as `Looma.Scene`
     * already does: TFunction is not Blueprint-expressible, so a public version would be
     * a C++-only API with no caller asking for it yet.
     */
    void FetchScenes(TFunction<void(bool bOk, const TArray<FLoomaSceneSummary>& Scenes)> OnDone);

    /**
     * `GET /performances` once, handing the rows to OnDone. The sibling of FetchScenes
     * and not a reuse of it: the two routes agree about the REQUEST and disagree about
     * the ANSWER. `/performances` replies with an OBJECT wrapping a `performances`
     * array where `/scenes` replies with a bare array, and the rows carry different
     * fields into different structs, so a shared fetch would have to hand back untyped
     * JSON and push the envelope and the typing back out to both callers — the
     * indirection without the benefit. What they genuinely share is the request, and
     * that is shared: see MakeCatalogueRequest.
     */
    void FetchPerformances(
        TFunction<void(bool bOk, const TArray<FLoomaPerformanceSummary>& Performances)> OnDone);

    /**
     * `GET /performances/{id}/cues` once, handing the rows to OnDone in running order.
     *
     * The third catalogue fetch, and the one that tested step 7's decision to extract
     * the REQUEST rather than a generic fetch: it needed no change to MakeCatalogueRequest
     * and shares no parsing with either sibling, because this envelope is `cues` and the
     * rows are a third shape. A generic fetch would have had to grow a third case here.
     *
     * Parameterised by a path segment, unlike the other two, and the segment is not
     * escaped: it comes from the hub's own `scene` frame, never from a caller — this
     * takes an INDEX, not an id. A command that accepts a performance id from a person
     * and builds a path from it is where escaping starts to earn its place.
     *
     * Read access, not edit: the route gates on `get_visible`, so a VIEWER may read the
     * running order (backend/app/performances.py). That is the common case rather than
     * the edge one, and it is right — stepping a timeline is watching, not editing.
     */
    void FetchCues(const FString& PerformanceId,
        TFunction<void(bool bOk, const TArray<FLoomaCue>& Cues)> OnDone);

    /**
     * Read one performance's running order, resolve a 0-based index against it, and
     * open that cue's scene. The single implementation behind both ways to fire a cue.
     *
     * Two callers with opposite preconditions, which is the seam: `Looma.Cue <index>`
     * REFUSES while a switch is in flight, because acting on the outgoing room's rail
     * is a race, while the post-switch jump fires at the one instant that race is over
     * — ConfirmRequestedPerformance has already cleared the pending id by the time it
     * calls this. Keeping the guard in the callers and the resolve here is what stops
     * the second one from being a copy that drifts.
     *
     * FailureNote is appended to every outcome that does not fire. It exists for one
     * caller: after a switch, "there is no cue 9" must not read as "the switch failed",
     * so that path passes a sentence saying the switch stands. `Looma.Cue` passes
     * nothing, because there is no other action for it to disclaim.
     */
    void ResolveAndOpenCue(const FString& PerformanceId, int32 CueIndex, const FString& FailureNote);

    /**
     * A GET against the backend, configured the one way a catalogue read has to be.
     *
     * Extracted because this is the half of a catalogue fetch that is easy to get wrong
     * and whose failure is SILENT: ApplyAuthHeader must come after SetURL or it does
     * nothing, and ApplyClientIdHeader is what anchors a guest to one subject, so
     * omitting either returns a short list rather than an error. A second route copying
     * fourteen lines by hand is exactly how one of them ends up missing a header and
     * quietly showing a guest somebody else's idea of "everything".
     */
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeCatalogueRequest(const FString& Url) const;

    /**
     * Compare what this socket asked for against what the `scene` frame confirms, once
     * per connection, and say which happened.
     *
     * Once per connection rather than per frame: every `openScene` reply is a `scene`
     * frame too, so a comparison made on every one of them would repeat the story of a
     * switch for the whole session. Only a socket that actually carried a request has
     * anything to confirm, which is what the latch encodes.
     */
    void ConfirmRequestedPerformance();

    /**
     * A close code that means START OVER, not TRY AGAIN. Stops the retry loop and
     * explains which of the two doors out of it applies.
     */
    void HandleTerminalClose(int32 Code, const FString& Reason);

    /** Spawn-or-update from one whole wire node (used by `scene` and `spawn`). */
    void UpsertNode(const TSharedPtr<FJsonObject>& Node);
    /** Apply a batch of whole nodes, parents first, then resolve any late parents. */
    void UpsertNodes(const TArray<TSharedPtr<FJsonValue>>& Nodes);
    /** Attach (or detach) an actor to its parent and set its parent-local pose. */
    void ApplyParent(ALoomaSyncedActor& Actor, const FString& ParentId, const FTransform& Local, bool bSnap);
    /**
     * Attach anything still waiting for a parent we hadn't seen yet. The hub orders
     * spawns parents-first, so this should never fire — it is the defensive half of
     * "a node whose parent is unknown becomes a root".
     */
    void ResolvePendingParents();
    /** Destroy an actor and forget it, without echoing a despawn back to the hub. */
    void DropNode(const FString& NodeId);
    /** What ApplyComponents needs from outside the node: the GLB url and the config. */
    FLoomaNodeRenderContext MakeRenderContext(const FLoomaNodeComponents& Components) const;
    /** The backend-relative GLB path a *web* peer loads, for a `model` we put on the wire. */
    FString MakeWebAssetUrl(const FString& AssetId) const;

    // Generation helpers
    void HydrateGenerationQueue();
    /** Store/replace a job and broadcast the matching events. */
    void ApplyJob(const FLoomaGenerationJob& Job);
    /** Fire-and-forget REST call; Body may be null. Results arrive over the WS. */
    void SendRest(const FString& Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body);

    UFUNCTION()
    void OnSyncedActorDestroyed(AActor* DestroyedActor);

    // Outbound motion diff
    void TickOutbound(float DeltaTime);
    void SendTransforms(const TArray<FString>& NodeIds, bool bTransient);
    /**
     * Report an attachment change made here: each node's new parent (null for a root)
     * with the pose it should keep under it. No transient/final split — a re-parent is
     * structural, it happens once per gesture, and there is nothing to stream.
     */
    void SendReparent(const TArray<FString>& NodeIds);

    /**
     * Two questions, in order, once a tick. First: has the local selection changed —
     * broadcast if so. Second, and only when connected: does the hub need telling.
     *
     * They are separate because they are answered against different baselines and have
     * different gates. Merging them is a real bug rather than a tidiness question: a
     * notification gated on connectivity goes silent exactly when a reconnect makes the
     * socket briefly absent, and one gated on the send diff fires on reconnect when
     * nothing local has changed at all.
     *
     * The send half keeps the same self-maintaining-baseline shape as TickOutbound's
     * pose diff and ALoomaSyncedActor::ParentId for attachment: one cached copy of what
     * the hub was last told, compared against the truth, with no separate dirty flag.
     * The one thing a pure diff cannot express is "the hub has forgotten", which is what
     * bForceSelectionSend is for.
     *
     * Called ahead of Tick's reconnect early-out, so the local half keeps working while
     * the socket is down.
     */
    void TickSelection();

    /**
     * The local selection as node ids, sorted and de-duplicated, compacting away any
     * actor that has since been destroyed.
     *
     * **This is the second place the selection changes**, and the easy one to miss: an
     * actor destroyed while selected leaves the set here, with no setter having been
     * called and nobody to call one. That is a consequence of holding the selection as
     * weak pointers — the design that makes destruction need no bookkeeping also makes
     * it invisible to any notify-at-the-setter scheme, which is why the change detector
     * in TickSelection compares state rather than trusting call sites.
     *
     * Sorted so both diffs are an array compare rather than a set compare, and so the
     * wire is deterministic — the contract treats `ids` as a set, so the order is ours
     * to choose and a stable one keeps a reordering from reading as a change.
     */
    TArray<FString> CollectSelectionIds();

#if WITH_EDITOR
    /**
     * The editor's actor selection changed: mirror whatever of it is ours into the
     * local selection, through SetLocalSelection like every other caller.
     *
     * Editor-only, and it only ever fires while PIE is running — not because of a
     * filter, but because ULoomaSceneSyncSubsystem is a UGameInstanceSubsystem and so
     * does not exist outside a running game. That is the answer to "why does clicking
     * in the Outliner before Play report nothing", and it is the behaviour we want:
     * dressing a level at design time is not a claim about what anyone is working on.
     *
     * The USelection* payload is ignored — see the implementation.
     */
    void OnEditorSelectionChanged(UObject* SelectionObject);

    /** Our binding on USelection::SelectionChangedEvent, released in Deinitialize. */
    FDelegateHandle EditorSelectionChangedHandle;
#endif

    /** Deliver the cached state to handles created for an already-known job. */
    void FlushPendingHandleReplays();

    TSharedPtr<IWebSocket> Socket;
    /** The URL the current socket was created with — what a reconnect compares against. */
    FString SocketUrl;
    /**
     * The guest name the current socket suggested in its `hello`, cleaned. Exactly
     * parallel to SocketUrl and there for the same reason: a settings edit has to be
     * able to tell whether the value actually moved, and the socket — not the settings
     * object — is what holds what we last told the hub.
     */
    FString SentDisplayName;
    /** Connect() has been called and the socket has neither opened nor failed yet. */
    bool bConnecting = false;
    /** What /health last told us about auth. Unknown until a probe lands, and again if one fails. */
    ELoomaAuthState AuthState = ELoomaAuthState::Unknown;
    /** Seconds until the next quiet re-probe; <= 0 means none is scheduled. */
    float HealthProbeCooldown = 0.0f;
    /**
     * The wait the *next* arming will use, doubling towards the cap. Zero means "not
     * backed off yet", so the first retry takes the floor rather than the cap.
     */
    float HealthProbeRetryDelay = 0.0f;
    /**
     * A quiet probe is outstanding. The retry cadence can be shorter than the request
     * timeout, so without this the loop would stack probes on a backend that is merely
     * slow — which is the same connection-pool contention it is trying to survive.
     */
    bool bHealthProbeInFlight = false;
    /**
     * The opaque session token from `POST /auth/login`, or empty. Memory only: nothing
     * writes it to disk, to a config, or to the log, and ApplyAuthHeader is the only
     * reader, so it never leaves this object as a value.
     */
    FString AuthToken;
    /** Who the backend last said we are. Kind == Unknown until something establishes it. */
    FLoomaIdentity CurrentIdentity;
    /**
     * Bumped by every AdoptSession / ClearSession. An in-flight auth request captures
     * it and drops its answer if it no longer matches — otherwise a `/auth/me` sent as
     * a guest could land after a login and overwrite the account identity with the
     * stale guest one. Cheaper and safer than capturing the token to compare: one
     * fewer copy of it in existence.
     */
    uint32 SessionSerial = 0;
    /** Our binding on ULoomaSceneSyncSettings::OnSettingsChanged, released on teardown. */
    FDelegateHandle SettingsChangedHandle;
    FString ClientId;
    TMap<FString, FLoomaTrackedActor> Tracked; // node id -> actor + last-sent cache
    TMap<FString, FLoomaGenerationJob> Jobs;   // jobId -> latest job snapshot

    /** Which saved scene the hub says is live (`sceneId`); empty for an unsaved one. */
    FString ActiveSceneId;

    /**
     * Which workspace the hub says this socket is in. Replaced whole by every `scene`
     * frame, never merged into — see HandleScene for why a null name must not be
     * papered over with the last one we knew.
     */
    FLoomaPerformance ActivePerformance;

    // Three pieces of performance state, not one, because "what we want", "what this
    // socket said" and "what the hub confirmed" are three different facts that are
    // routinely all different at once — for the whole of a switch, which is a reconnect
    // and therefore not instant.

    /**
     * The performance the user asked for, or empty for "wherever the hub puts us".
     * Survives the socket by design: it is an intent, so every reconnect — a retry, a
     * `Looma.Reconnect`, a backend-address change — carries it again. Only
     * SwitchPerformance moves it, including after a refusal, because quietly revising
     * where somebody asked to be is the one thing a client must not do about this.
     */
    FString RequestedPerformanceId;

    /**
     * What THIS socket's `hello` actually carried. Snapshotted in Connect() beside
     * SentDisplayName and for the same reason, with a sharper edge: this one is
     * compared against the hub's answer later, so a request that moved between making
     * the socket and the `scene` frame landing would otherwise make us report a
     * mismatch that never happened.
     */
    FString SentPerformanceId;

    /**
     * This socket carried a request and no `scene` frame has answered it yet. The
     * window in which ActivePerformance is one socket out of date while looking
     * entirely valid, which is why GetPendingPerformanceId exists to be shown beside it.
     */
    bool bAwaitingPerformanceConfirmation = false;

    /**
     * A cue to open once the switch lands, or INDEX_NONE. The only piece of intent here
     * that has to survive a reconnect, because the action it describes cannot happen
     * until the socket it needs has been replaced.
     *
     * Dropped by everything that makes it meaningless, each handled where that happens:
     * a terminal 4400/4403/4404 close, since the socket it was waiting for will never
     * open; a plain `Looma.Performance <id>` with no index, which supersedes it (which
     * is why SwitchPerformance clears it and SwitchPerformanceToCue arms it after
     * calling through); and landing anywhere other than the performance asked for,
     * where an index chosen against one running order would otherwise be applied to a
     * different one the user has never seen. It survives a plain reconnect on purpose:
     * a retry, or a `Looma.Reconnect` typed to hurry one along, is the same switch
     * still trying to happen.
     */
    int32 PendingCueIndex = INDEX_NONE;

    /**
     * The local selection, held **weakly and as actors** rather than as node ids.
     *
     * The wire wants ids, so storing ids and validating them against `Tracked` at send
     * time was the other option. Weak actor pointers win because they make the
     * awkward case disappear instead of handling it: an actor destroyed while selected
     * leaves the selection with no bookkeeping, no destruction hook, and no window in
     * which the stored set and the world disagree. Ids would need a validation pass
     * against `Tracked` — which is itself keyed by id and holds its own weak pointer,
     * so it is a second indirection to the same truth — and an id that was never a
     * node at all would sit in the selection forever.
     */
    TArray<TWeakObjectPtr<ALoomaSyncedActor>> LocalSelection;

    /**
     * The ids the last `selection` carried — the *send* baseline. Sorted, so comparing
     * is an array compare.
     */
    TArray<FString> LastSentSelectionIds;

    /**
     * The ids OnLocalSelectionChanged last announced — the *notify* baseline.
     *
     * A second baseline rather than a reuse of the one above, because the two answer
     * different questions: "what does the hub believe" and "what have local listeners
     * been told". They diverge whenever the socket is down (local changes, hub told
     * nothing) and again on reconnect (hub re-told, nothing local changed), which is
     * precisely the pair of bugs sharing one baseline produced.
     */
    TArray<FString> LastNotifiedSelectionIds;

    /**
     * Send the next selection even if the diff says it has not moved.
     *
     * Set on connect, because a diff alone cannot know the hub has forgotten us: a
     * reconnecting client "comes back with an empty selection" and its send-on-connect
     * is what restores the claim (docs/scene-format.md). Clearing the baseline instead
     * would not do — if our selection is *also* empty the diff would suppress the
     * message, and the contract says send it on connect regardless. This matters more
     * than it used to: HAM-181 made a login and a logout each reconnect, so connect
     * happens several times in an ordinary session, not once at startup.
     */
    bool bForceSelectionSend = false;

    /**
     * The room, from the last `clients` roster: everyone but us, in join order.
     *
     * A flat array rather than a map keyed by id, though GetClient() then has to scan
     * it. Order is load-bearing — it is the claim tiebreak step 2 resolves conflicts
     * with, and every client receives hub messages in the same order, so preserving it
     * is what makes all of them agree without a byte of extra protocol. A TMap would
     * have thrown it away for a lookup on a set whose size is a handful of people in a
     * room.
     *
     * Kept next to the selection state above rather than beside the scene state,
     * because its lifetime is the selection's, not the scene's: the scene document
     * survives a reconnect and is re-reconciled from the hub's `scene`, while this
     * dies with the socket outright — see ClearPresence.
     */
    TArray<FLoomaClient> RemoteClients;

    /**
     * Our own roster entry, held apart from RemoteClients so nothing that iterates
     * that list can draw our colour on our own selection. Id is empty when we have no
     * self entry — before the first roster, or if the hub sent one that does not name
     * us at all.
     */
    FLoomaClient SelfClient;

    /**
     * The roster's `you`: the id the hub says is ours. Empty until a roster lands.
     *
     * Stored rather than assumed equal to ClientId, even though the hub echoes back
     * the id we sent in `hello`. The message is built per recipient and `you` is how
     * the contract says a client finds itself; deriving it from ClientId instead would
     * mean we could never notice the two disagreeing, and a disagreement is precisely
     * the case where we would start painting our own colour on our own selection.
     * HandleClients cross-checks them and warns.
     */
    FString OwnClientId;

    /**
     * The claim ledger: nodeId -> claimants, oldest claim first. A node with no
     * claimants has no entry at all, so a lookup miss and an empty list never both
     * mean "unclaimed".
     *
     * Keyed by NODE and only by node, though step 3 needs both directions — "who owns
     * this node" and "every node this client draws". The second direction is already
     * held, once, as FLoomaClient::Selection: it is what the wire sends and what the
     * roster carries, so a per-client index here would be a second copy of a fact the
     * room already stores, kept in step by hand. The reverse mapping is therefore free
     * (GetClientBorderNodes filters that client's own selection through this map, one
     * O(1) lookup per id) and the price is paid in exactly one place: removing a
     * departed client scans every list, once per roster, over the handful of nodes a
     * room has selected. Cheap where it is rare, free where it is hot.
     *
     * There is deliberately no cached "owner per node" alongside it. The owner IS the
     * head of a list — deriving it is an array index, and a maintained second copy of
     * a one-element derivation is a thing that can disagree with its source.
     */
    TMap<FString, TArray<FString>> Claims;

    /** What the last recompute decided to draw, in roster order. See GetRemoteBorderGroups. */
    TArray<FLoomaBorderGroup> BorderGroups;

    /** Clients that hold a border and did not get a slot. See GetUndrawnClients. */
    TArray<FString> UndrawnClientIds;

    /**
     * Every primitive we currently have a non-zero stencil value on.
     *
     * Held weakly and kept explicitly, because clearing is the half that goes wrong:
     * a node that loses its border has to be restored to
     * `SetRenderCustomDepth(false)`, and without this the only way to find it would be
     * to sweep every actor in the world — which would also stamp on a stencil value
     * the consumer project set for its own reasons. Weak, so an actor destroyed while
     * claimed simply drops out; nothing here ever dereferences a stale pointer, and
     * the ledger itself holds node ids rather than pointers.
     */
    TSet<TWeakObjectPtr<UPrimitiveComponent>> StencilComponents;

    /** The collection resolved from the setting, held so it is not collected under us. */
    UPROPERTY()
    TObjectPtr<UMaterialParameterCollection> ResolvedBorderCollection;

    /** Set by MarkBordersDirty, spent by TickBorders. */
    bool bBordersDirty = false;

    /**
     * One-shot latches for the two diagnostics that would otherwise print every frame.
     * Both reset when the condition clears, so a fixed setting says so once and a
     * regression is reported again rather than swallowed.
     */
    bool bWarnedNoBorderCollection = false;
    bool bWarnedCustomDepthOff = false;

    /** jobId -> per-job event handle. Only jobs a caller asked about get one. */
    UPROPERTY()
    TMap<FString, TObjectPtr<ULoomaGenerationHandle>> JobHandles;

    /** jobId -> suggested spawn pose, once learned. Never changes for a job. */
    TMap<FString, FTransform> SuggestedTransforms;

    /** Handles awaiting their first replay; drained at the top of Tick. */
    TArray<FString> PendingHandleReplays;

    bool bApplyingRemote = false;              // suppress outbound while applying inbound
    float ReconnectCooldown = 0.0f;
    /**
     * The close code that stopped us, or 0. Non-zero means no retry is armed and none
     * will be: the handshake was refused in a way that repeating it cannot fix.
     *
     * The code and not a bool, so `Looma.Status` can say WHICH refusal without the
     * answer living only in a log line that has since scrolled away. Cleared by
     * Connect(), so making a socket is what supersedes it and no caller has to
     * remember to.
     */
    int32 TerminalCloseCode = 0;
    float SinceLastTransientSend = 1.0f;
};
