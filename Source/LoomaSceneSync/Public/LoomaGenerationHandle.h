#pragma once

#include "CoreMinimal.h"
#include "LoomaGenerationTypes.h"
#include "UObject/Object.h"
#include "LoomaGenerationHandle.generated.h"

class ALoomaSyncedActor;
class ULoomaSceneSyncSubsystem;

/** Per-job counterpart of the subsystem-wide FLoomaGenerationJobEvent. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaJobHandleEvent, const FLoomaGenerationJob&, Job);

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

    /** False until the first Apply — lets the subsystem skip a redundant replay. */
    bool bHasApplied = false;
};
