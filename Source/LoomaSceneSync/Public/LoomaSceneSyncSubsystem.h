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

    /** Take on a session: the token and the identity it belongs to, which only mean anything together. */
    void AdoptSession(const FString& Token, const FLoomaIdentity& NewIdentity);

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

    /** Deliver the cached state to handles created for an already-known job. */
    void FlushPendingHandleReplays();

    TSharedPtr<IWebSocket> Socket;
    /** The URL the current socket was created with — what a reconnect compares against. */
    FString SocketUrl;
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
