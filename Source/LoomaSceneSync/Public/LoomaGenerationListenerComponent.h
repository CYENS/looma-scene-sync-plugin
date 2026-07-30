#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "LoomaGenerationHandle.h"
#include "LoomaGenerationTypes.h"
#include "LoomaGenerationListenerComponent.generated.h"

class ULoomaSceneSyncSubsystem;

// The five per-stage delegates are reused verbatim from ULoomaGenerationHandle, so
// an event node dropped from this component has exactly the pins it would have had
// from a manual Bind Event. Only the two below are new: the handle carries OnUpdated,
// OnFailed and OnCancelled as raw state events (Job only), and the point of this
// component is that every event opens with the same JobId / Job / JobHandle triple.

/** The job failed (State == Failed). Error is the backend's reason. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FLoomaJobFailedStageEvent,
    const FString&, JobId, const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle,
    const FString&, Error);

/**
 * Just the triple, for the events that add nothing to it: OnUpdated (everything
 * worth reading is already a field of Job) and OnCancelled (nothing survives).
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FLoomaJobStageEvent,
    const FString&, JobId, const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle);

/**
 * Follow one generation job from an actor's event graph, without writing a single
 * Bind Event node.
 *
 * Add this component to your orb/placeholder actor, select it in the Components
 * panel, and every event below appears in the Details panel with a `+` that drops a
 * pre-bound red event node into the graph — the same one click that gives you
 * `On Component Begin Overlap`. That is as close to `Event BeginPlay` as Unreal
 * gets without forcing your actor onto a particular base class.
 *
 * Point it at a job with `Watch` (you have the handle — e.g. straight off
 * `Submit Generation`'s Handle pin) or `Watch Job Id` (you only have the id). Both
 * are safe to call before the job exists on the hub: the handle replays its cached
 * state on the next tick, so a submit -> watch -> bind sequence never misses the
 * `queued` event even when it beats the POST response over the socket.
 *
 * Cadence is inherited from the handle and differs by intent:
 *   - OnUpdated, OnQueueUpdate, OnGeneratingImagesUpdate and OnGeneratingAssetUpdate
 *     fire on EVERY snapshot, so queue positions and progress keep ticking — wire
 *     them to a progress ring. OnUpdated is the catch-all: it fires for every
 *     snapshot in every state, ahead of whichever stage event also applies.
 *   - OnAwaitingImageSelection, OnAssetGenerated, OnFailed and OnCancelled fire ONCE,
 *     on the transition in. That matters: the hub re-announces jobs and replays the
 *     whole queue on reconnect, so an every-snapshot OnAssetGenerated would spawn
 *     the asset twice.
 *
 * Watching a second job unbinds the first, and the component unbinds itself on
 * EndPlay — handles outlive actors (they live as long as the subsystem), so a
 * destroyed orb must not keep receiving events.
 */
UCLASS(ClassGroup = (Looma), meta = (BlueprintSpawnableComponent, DisplayName = "Looma Generation Listener"))
class LOOMASCENESYNC_API ULoomaGenerationListenerComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULoomaGenerationListenerComponent();

    // --- Events ---------------------------------------------------------------

    /**
     * Any change at all to this job — state, progress, queue position, enhanced
     * prompt. Fires on EVERY snapshot, before whichever stage event that snapshot
     * also triggers.
     *
     * This is a superset of every other event here, so do not wire it to work that
     * must happen once: a snapshot that finishes the job fires both this and
     * OnAssetGenerated. Use it for things that are naturally idempotent — refreshing
     * a label, driving a progress ring — and the stage events for everything else.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobStageEvent OnUpdated;

    /**
     * Sitting in the queue, waiting for the GPU (State == Queued). Fires on every
     * snapshot, so QueuePosition ticks down as jobs ahead finish.
     *
     * Fires for BOTH queue waits — before the candidate images are rendered and
     * again after a candidate is picked. `Phase` tells you which.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobQueueEvent OnQueueUpdate;

    /**
     * Rendering the candidate images (State == Running, nothing picked yet). Fires
     * on every snapshot; JobProgress is 0..1 over this stage alone.
     *
     * JobEnhancedPrompt is empty until the backend produces one, and can start
     * arriving mid-stage — itself a change that re-fires this event.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobGeneratingImagesEvent OnGeneratingImagesUpdate;

    /**
     * Candidates are ready and the job is parked until someone picks one
     * (State == AwaitingImage). Fires once per entry — a Regenerate legitimately
     * re-enters this stage and fires it again.
     *
     * Feed a JobImages entry's Url to `Download Image As Texture`, then pass its Id
     * to JobHandle's `Select Image`.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobAwaitingImageSelectionEvent OnAwaitingImageSelection;

    /**
     * Reconstructing the 3D asset from the picked candidate (State == Running, after
     * a selection). Fires on every snapshot; JobProgress restarts from 0 here.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobGeneratingAssetEvent OnGeneratingAssetUpdate;

    /**
     * The asset exists in the datalake (State == Done). Fires once.
     *
     * Nothing is placed for you — call JobHandle's `Spawn Synced Asset` to put an
     * instance in the world (and on every peer), then destroy your placeholder.
     */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobAssetGeneratedEvent OnAssetGenerated;

    /** The job failed (State == Failed); Error is set. Despawn the orb here. */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobFailedStageEvent OnFailed;

    /** The job was cancelled, by this client or another (State == Cancelled). */
    UPROPERTY(BlueprintAssignable, Category = "Looma|Generation")
    FLoomaJobStageEvent OnCancelled;

    // --- Pointing it at a job -------------------------------------------------

    /**
     * Follow this handle, releasing whatever was being watched before. Null just
     * stops watching. Safe to call on an in-flight job — the handle replays its
     * cached state next tick.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void Watch(ULoomaGenerationHandle* Handle);

    /**
     * Follow the job with this id, resolving the handle through the subsystem
     * (minting it if this is the first ask). Returns the handle so you can store it,
     * or null if the subsystem is unavailable or JobId is empty.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation", meta = (DisplayName = "Watch Job Id"))
    ULoomaGenerationHandle* WatchJobId(const FString& JobId);

    /** Unbind from the current job. Events stop; the handle itself is untouched. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Generation")
    void StopWatching();

    /** The handle currently being followed, if any. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    TObjectPtr<ULoomaGenerationHandle> WatchedHandle;

    UFUNCTION(BlueprintPure, Category = "Looma|Generation")
    bool IsWatching() const;

    /**
     * Latest snapshot of the watched job, or a default-constructed job if nothing is
     * being watched. Every event already hands you this on a pin — use this only
     * when you need the job outside an event, e.g. from Tick.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Generation")
    FLoomaGenerationJob GetJob() const;

    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    // Re-broadcast shims. These exist only because dynamic multicast delegates need
    // a UFUNCTION on the receiving object; each one forwards its pins unchanged.
    UFUNCTION()
    void HandleQueueUpdate(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* JobHandle, int32 QueuePosition, ELoomaQueuePhase Phase);

    UFUNCTION()
    void HandleGeneratingImages(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* JobHandle, float JobProgress, const FString& JobPrompt,
        const FString& JobEnhancedPrompt);

    UFUNCTION()
    void HandleAwaitingImageSelection(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* JobHandle, const TArray<FLoomaGeneratedImage>& JobImages);

    UFUNCTION()
    void HandleGeneratingAsset(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* JobHandle, const FLoomaGeneratedImage& SelectedImage, float JobProgress);

    UFUNCTION()
    void HandleAssetGenerated(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* JobHandle, const FString& AssetId);

    // The handle carries these three as raw state events, so they arrive with only
    // the job — the JobId / JobHandle half of the triple is filled in from what we
    // hold.
    UFUNCTION()
    void HandleUpdated(const FLoomaGenerationJob& Job);

    UFUNCTION()
    void HandleFailed(const FLoomaGenerationJob& Job);

    UFUNCTION()
    void HandleCancelled(const FLoomaGenerationJob& Job);

    void BindTo(ULoomaGenerationHandle* Handle);
    void UnbindFrom(ULoomaGenerationHandle* Handle);

    ULoomaSceneSyncSubsystem* GetSubsystem() const;
};
