#pragma once

#include "CoreMinimal.h"
#include "LoomaAuthTypes.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSyncedActor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "LoomaSceneSyncSubsystem.generated.h"

class IWebSocket;
class IHttpRequest;
class FJsonObject;
class FJsonValue;
class ULoomaGenerationHandle;

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
 * the sender, so all of them are applied idempotently.
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

    /** jobId -> per-job event handle. Only jobs a caller asked about get one. */
    UPROPERTY()
    TMap<FString, TObjectPtr<ULoomaGenerationHandle>> JobHandles;

    /** jobId -> suggested spawn pose, once learned. Never changes for a job. */
    TMap<FString, FTransform> SuggestedTransforms;

    /** Handles awaiting their first replay; drained at the top of Tick. */
    TArray<FString> PendingHandleReplays;

    bool bApplyingRemote = false;              // suppress outbound while applying inbound
    float ReconnectCooldown = 0.0f;
    float SinceLastTransientSend = 1.0f;
};
