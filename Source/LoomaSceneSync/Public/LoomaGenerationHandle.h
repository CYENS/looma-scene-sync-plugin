#pragma once

#include "CoreMinimal.h"
#include "LoomaGenerationTypes.h"
#include "UObject/Object.h"
#include "LoomaGenerationHandle.generated.h"

class ALoomaSyncedActor;
class ULoomaGenerationHandle;
class ULoomaSceneSyncSubsystem;

/** Per-job counterpart of the subsystem-wide FLoomaGenerationJobEvent. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaJobHandleEvent, const FLoomaGenerationJob&, Job);

// --- Per-stage events ------------------------------------------------------
// Higher-level than the raw state events above: one per stage a job actually
// goes through, each carrying the parameters that matter at that stage on its
// own pin so a Blueprint never has to break out the Job struct.
//
// All five open with the same three parameters — JobId, Job, JobHandle — so they
// are interchangeable at the call site, and the handle is on the pin list so an
// event graph can drive the job (Select Image / Spawn Synced Asset / Cancel)
// without having stored the handle anywhere.

/** Queued, waiting for the GPU. Phase says whether for images or for the 3D asset. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FLoomaJobQueueEvent,
    const FString&, JobId, const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle,
    int32, QueuePosition, ELoomaQueuePhase, Phase);

/** Running, rendering the candidate images (before any selection). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_SixParams(FLoomaJobGeneratingImagesEvent,
    const FString&, JobId, const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle,
    float, JobProgress, const FString&, JobPrompt, const FString&, JobEnhancedPrompt);

/** Candidates are ready to pick. Call the handle's Select Image with one of their ids. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FLoomaJobAwaitingImageSelectionEvent,
    const FString&, JobId, const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle,
    const TArray<FLoomaGeneratedImage>&, JobImages);

/** Running, reconstructing the 3D asset from the picked candidate. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FLoomaJobGeneratingAssetEvent,
    const FString&, JobId, const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle,
    const FLoomaGeneratedImage&, SelectedImage, float, JobProgress);

/** The asset is in the datalake. Place it with the handle's Spawn Synced Asset. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FLoomaJobAssetGeneratedEvent,
    const FString&, JobId, const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle,
    const FString&, AssetId);

/**
 * One generation job's events, scoped to that job.
 *
 * The subsystem's OnGenerationJob* events fire for EVERY job on the hub —
 * including jobs the web app submitted — so a Blueprint bound to those has to
 * filter on JobId itself, and gets a burst of events for historical jobs when
 * the queue hydrates on connect. Bind a handle instead and you only ever hear
 * about this one job.
 *
 * Get one from `Submit Generation` (the Handle output pin, next to JobId) or
 * from `Get Generation Handle` on the subsystem. Handles live as long as the
 * subsystem, so storing one in a variable on your orb actor is safe.
 *
 * Event semantics: OnUpdated fires on every change (progress ticks included);
 * the state events fire once, on the TRANSITION into that state, so a duplicate
 * delivery (a WS event and the REST hydrate carrying the same state) won't
 * double-fire them. `Job` always holds the latest snapshot.
 *
 * Two families of events are offered. The raw state events (OnUpdated /
 * OnImagesReady / OnDone / OnFailed / OnCancelled) hand you the job struct and
 * leave the reading to you. The per-stage events below mirror the stages a job
 * actually goes through, each with the values that matter at that stage broken
 * out onto their own pins. They are additive — the raw events still fire exactly
 * as before, so nothing bound to them needs changing.
 *
 * Per-stage cadence differs by intent:
 *   - OnQueueUpdate, OnGeneratingImagesUpdate and OnGeneratingAssetUpdate fire on
 *     EVERY snapshot while the job is in that stage, so queue positions and
 *     progress ticks keep arriving — bind them to a progress ring.
 *   - OnAwaitingImageSelection and OnAssetGenerated fire ONCE, on the transition
 *     in, like the raw state events. That matters: the hub re-announces jobs
 *     (`_announce_job`) and replays the whole queue on reconnect, so an
 *     every-snapshot OnAssetGenerated would spawn the asset twice.
 *
 * Binding is race-free: a handle for a job that is already in flight replays
 * the cached state on the next tick, which is after the Blueprint that just
 * created it has finished binding. So `Submit Generation` -> bind -> done never
 * misses an event, even though the first `queued` event can arrive over the
 * socket before the POST response carrying the job id.
 */
UCLASS(BlueprintType)
class LOOMASCENESYNC_API ULoomaGenerationHandle : public UObject
{
    GENERATED_BODY()

public:
    /** Any change to this job — state, progress, queue position, enhanced prompt. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobHandleEvent OnUpdated;

    /** Candidate images are ready to pick (State == AwaitingImage). Call SelectImage. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobHandleEvent OnImagesReady;

    /** The model is ready (State == Done); Job.AssetId / Job.AssetUrl are populated. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobHandleEvent OnDone;

    /** The job failed (State == Failed); Job.Error is set. Despawn the orb here. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobHandleEvent OnFailed;

    /** The job was cancelled, by this client or another (State == Cancelled). */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobHandleEvent OnCancelled;

    // --- Per-stage events ----------------------------------------------------
    // See the class comment for how these differ from the state events above.

    /**
     * Sitting in the queue, waiting for the GPU (State == Queued). Fires on every
     * snapshot, so QueuePosition ticks down as jobs ahead finish.
     *
     * Fires for BOTH queue waits: before the candidate images are rendered and
     * again after a candidate is picked. `Phase` tells you which.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobQueueEvent OnQueueUpdate;

    /**
     * Rendering the candidate images (State == Running, nothing picked yet). Fires
     * on every snapshot; JobProgress is 0..1 over this stage alone.
     *
     * JobEnhancedPrompt is empty until the backend produces one, and it can start
     * arriving mid-stage — which is itself a change that re-fires this event.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobGeneratingImagesEvent OnGeneratingImagesUpdate;

    /**
     * Candidates are ready and the job is parked until someone picks one
     * (State == AwaitingImage). Fires once per entry into the stage — a Regenerate
     * legitimately re-enters it, and fires it again.
     *
     * Feed a JobImages entry's Url to `Download Image As Texture`, then pass its
     * Id to this handle's `Select Image`.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobAwaitingImageSelectionEvent OnAwaitingImageSelection;

    /**
     * Reconstructing the 3D asset from the picked candidate (State == Running,
     * after a selection). Fires on every snapshot; JobProgress restarts from 0 for
     * this stage.
     *
     * SelectedImage is resolved by matching Job.SelectedImageUrl against
     * Job.Images, which the backend keeps populated through this stage. If no
     * candidate matches, only its Url is set (Id empty, Seed 0).
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobGeneratingAssetEvent OnGeneratingAssetUpdate;

    /**
     * The asset exists in the datalake (State == Done). Fires once.
     *
     * Nothing is placed for you — call this handle's `Spawn Synced Asset` to put an
     * instance in the world (and on every peer), then destroy your placeholder.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobAssetGeneratedEvent OnAssetGenerated;

    /** The job this handle follows. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString JobId;

    /** Latest snapshot. Valid before any event fires if the job was already known. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FLoomaGenerationJob Job;

    /** True once the job reached a terminal state (Done / Failed / Cancelled). */
    UFUNCTION(BlueprintPure, Category = "Looma|Generation")
    bool IsFinished() const;

    // --- Drive this job (no JobId to thread through) --------------------------

    /** Pick a candidate image, starting 3D reconstruction. POST /generate/{id}/select. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void SelectImage(const FString& ImageId);

    /** Re-roll the candidate images. Empty Prompt / NImages <= 0 keep the current values. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void Regenerate(const FString& Prompt, int32 NImages = 0);

    /** Cancel this job. DELETE /generate/{id}. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void Cancel();

    /**
     * Place an instance of this job's finished asset, here and on every connected
     * peer. The subsystem call with this job's AssetId and JobId filled in, so a
     * Blueprint bound to OnAssetGenerated needs neither the subsystem nor an id.
     *
     * Tagging the instance with the JobId is what makes `Find Spawned Actor`
     * resolve it later. Returns null before the job is Done (no AssetId yet).
     *
     * The pose is yours to choose: `Job.SuggestedTransform` if you want to honour
     * what the submitter asked for (check `Job.bHasSuggestedTransform` first — it
     * is advisory and often absent), or anywhere else. An empty Name falls back to
     * the asset id.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    ALoomaSyncedActor* SpawnSyncedAsset(const FString& Name, const FTransform& Transform);

    /**
     * The synced actor a client placed for this job, if any — i.e. one spawned via
     * `Spawn Synced Asset` with this JobId. Nothing places it automatically; the
     * suggested pose is only advice. Null until someone acts on it.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    ALoomaSyncedActor* FindSpawnedActor() const;

private:
    friend class ULoomaSceneSyncSubsystem;

    /** Store the snapshot and fire the matching events. Called by the subsystem. */
    void Apply(const FLoomaGenerationJob& InJob);

    /**
     * Record the suggested spawn pose before any event arrives, so
     * `Job.SuggestedTransform` / `Job.bHasSuggestedTransform` are readable the
     * moment you get the handle. Driven by the subsystem's NoteSuggestedTransform — the backend's
     * first `generation` event is emitted before it has stored the pose, so it
     * would otherwise report none.
     */
    void SeedTransform(const FTransform& InTransform);

    /** The owning subsystem (this object's outer). */
    ULoomaSceneSyncSubsystem* GetSubsystem() const;

    /**
     * Recompute bImageSelected from the snapshot just stored in `Job`, given the
     * state we were in before it. See that field for why this isn't a plain read
     * of Job.SelectedImageUrl.
     */
    void UpdateImageSelected(ELoomaJobState PreviousState);

    /** Which stage the current Queued/Running snapshot belongs to. */
    ELoomaQueuePhase GetPhase() const;

    /**
     * The candidate matching Job.SelectedImageUrl, or a Url-only stand-in if none
     * does (the manifest can be absent on a job hydrated mid-flight).
     */
    FLoomaGeneratedImage FindSelectedImage() const;

    /** False until the first Apply — lets the subsystem skip a redundant replay. */
    bool bHasApplied = false;

    /**
     * True once this job's candidate has been picked — i.e. Queued/Running now mean
     * the 3D stage, not the image stage. The backend reuses both states for both
     * stages, so this is the only thing separating them.
     *
     * Not simply `!Job.SelectedImageUrl.IsEmpty()`, for two reasons:
     *  - The pick is recorded in SQLite *after* `select()` has already transitioned
     *     the job to queued and broadcast it, so the first post-selection snapshot
     *     carries no image url. SelectImage() sets this optimistically to cover it.
     *  - `regenerate` does not clear the recorded pick, so a job that failed during
     *     3D and was re-rolled carries a stale url back into the image stage.
     *     Re-entering AwaitingImage — or leaving Failed for Queued, which only a
     *     regenerate does — clears this again.
     *
     * A remote observer that did not itself call SelectImage can still misread the
     * single racy snapshot above as the image stage; the next one corrects it.
     */
    bool bImageSelected = false;
};
