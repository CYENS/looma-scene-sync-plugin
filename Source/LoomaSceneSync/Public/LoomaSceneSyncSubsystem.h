#pragma once

#include "CoreMinimal.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSyncedActor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "LoomaSceneSyncSubsystem.generated.h"

class IWebSocket;
class FJsonObject;
class FJsonValue;
class ULoomaGenerationHandle;

/** Per-node bookkeeping for the outbound motion diff. */
struct FLoomaTrackedActor
{
    TWeakObjectPtr<ALoomaSyncedActor> Actor;
    /** Parent-local, like everything on the wire — a child's diff must not see its parent's motion. */
    FTransform LastSent;
    int32 StillFrames = 0;
    bool bMoving = false;
};

/** Fired for generation-job lifecycle changes. Carries the full job snapshot. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaGenerationJobEvent, const FLoomaGenerationJob&, Job);

/** Fired when the scene-sync socket connects / disconnects. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FLoomaSyncConnectionEvent);

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
 * rest. Works in packaged builds (no editor delegates).
 *
 * Wire convention: right-handed, Y-up, meters, quaternion [x,y,z,w]
 * (three.js/glTF native). Conversion to UE (left-handed, Z-up, cm) is
 * (x,y,z) -> (-z,x,y) x100, matching glTFRuntime's DEFAULT SceneBasis
 * (FglTFRuntimeConfig::GetMatrix) so meshes and transforms agree. It stays valid
 * per node under hierarchy: a change of basis composes across a parent chain
 * (M(L1 L2)M^-1 = (M L1 M^-1)(M L2 M^-1)), so each node's local transform converts
 * independently and UE attachment composes the world pose as three.js does.
 */
UCLASS(Config = Game)
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

    /** REST/asset base for direct (non-proxied) access, e.g. "http://127.0.0.1:8000". */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString GetRestBase() const;

    /**
     * Turn a backend-relative path into an absolute URL a native client can hit.
     * Passes absolute (http...) URLs through unchanged; strips a leading "/api"
     * (the web proxy prefix) so "/api/static/x.png" -> "http://<host>/static/x.png".
     */
    UFUNCTION(BlueprintPure, Category = "Looma")
    FString ResolveBackendUrl(const FString& PathOrUrl) const;

    /**
     * host:port of the FastAPI backend. Override in Config/DefaultGame.ini:
     *   [/Script/LoomaSceneSync.LoomaSceneSyncSubsystem]
     *   BackendHost=192.168.1.10:8000
     * Prefer an explicit IP over "localhost": UE's WebSocket client may
     * resolve localhost to IPv6 (::1) while uvicorn listens on IPv4 only.
     */
    UPROPERTY(Config)
    FString BackendHost = TEXT("127.0.0.1:8000");

    /**
     * Trim on every light's wire intensity. The units already match 1:1 (three.js is
     * in candela for point/spot and lux for directional, and so is UE), so this exists
     * only because the two renderers' exposure and tone mapping differ — see
     * ALoomaSyncedActor::ApplyLight. Same .ini section as BackendHost.
     */
    UPROPERTY(Config)
    float LightIntensityScale = 1.0f;

    /**
     * Lift a `model`'s GLB so its bounding-box floor sits on the node origin, matching
     * the web client (frontend/src/scene/pivot.js). False restores the pre-v3 Unreal
     * behaviour — the GLB centred on the node origin, i.e. half-buried at floor level.
     */
    UPROPERTY(Config)
    bool bBaseAlignModels = true;

private:
    void Connect();
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

    /** Deliver the cached state to handles created for an already-known job. */
    void FlushPendingHandleReplays();

    TSharedPtr<IWebSocket> Socket;
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
