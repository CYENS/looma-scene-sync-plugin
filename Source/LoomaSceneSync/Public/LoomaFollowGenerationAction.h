#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "LoomaGenerationTypes.h"
#include "LoomaFollowGenerationAction.generated.h"

class ULoomaGenerationHandle;
class ULoomaSceneSyncSubsystem;
class ULoomaSubmitGenerationAction;

/**
 * Every exec pin on the node below carries these same two values.
 *
 * Deliberately uniform: an async node's data pins are the union of all its
 * delegates' parameters, and a pin belonging to a delegate that did not fire reads
 * as a default value on that path. One shared signature means every pin on the node
 * is meaningful no matter which exec pin fired. Which stage you are in is carried by
 * the pin itself, and everything a stage exposes is a field of Job.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FLoomaFollowGenerationEvent,
    const FLoomaGenerationJob&, Job, ULoomaGenerationHandle*, JobHandle);

/**
 * One node, one exec pin per stage of a generation job.
 *
 * The convenience counterpart to ULoomaGenerationListenerComponent: use this when
 * the whole flow lives in one graph and reads top-to-bottom, and the component when
 * the reaction belongs to an actor (an orb that shows progress and replaces itself
 * with the finished model).
 *
 * Two ways in. `Submit Generation And Follow` starts a job and follows it in a
 * single node. `Follow Generation` attaches to a handle you already have — from
 * `Submit Generation`, from the subsystem's `Get Generation Handle`, or from another
 * client's job you found via `Get All Generation Jobs`.
 *
 * WHICH Job FIELDS MATTER, PER PIN — the rest are still readable, just not the point:
 *
 *   On Updated               anything may have moved; Job.State says where the job
 *                            actually is. The catch-all, for when you would rather
 *                            switch on the state yourself than use the pins below.
 *   On Queued For Images     Job.QueuePosition (1-based), Job.Prompt
 *   On Generating Images     Job.Progress (0..1 over this stage), Job.EnhancedPrompt
 *   On Awaiting Image Sel.   Job.Images — each has Id, Url, Seed. Feed a Url to
 *                            `Download Image As Texture`, then pass its Id to
 *                            JobHandle's `Select Image` to continue.
 *   On Queued For Asset      Job.QueuePosition, Job.SelectedImageUrl
 *   On Generating Asset      Job.Progress (restarts from 0), Job.SelectedImageUrl
 *   On Asset Generated       Job.AssetId, Job.AssetUrl. Call JobHandle's
 *                            `Spawn Synced Asset` to place it here and on every peer.
 *   On Failed                Job.Error
 *   On Cancelled             nothing in particular; the job is gone
 *
 * The two queue pins exist so the stage is legible without a Phase parameter: the
 * backend puts a job back in the queue after a candidate is picked, so `Queued`
 * alone is ambiguous.
 *
 * CADENCE. On Updated, On Queued For Images, On Generating Images, On Queued For
 * Asset and On Generating Asset fire on EVERY snapshot, so progress and queue
 * position keep arriving — treat them like Tick, not like a one-shot. The other four
 * fire ONCE, on the transition in, which is what keeps On Asset Generated from
 * spawning the model twice when the hub replays its queue on reconnect.
 *
 * LIFETIME. The node releases itself after On Asset Generated or On Cancelled. It
 * deliberately does NOT release after On Failed: a failed job can be re-rolled with
 * JobHandle's `Regenerate`, and the graph should keep following it when that happens.
 * A failed job nobody re-rolls is reclaimed with the game instance.
 */
UCLASS()
class LOOMASCENESYNC_API ULoomaFollowGenerationAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /**
     * Any change at all to the job, in any state. Fires on every snapshot, ahead of
     * whichever stage pin that same snapshot also fires.
     *
     * A superset of every other pin here — the snapshot that finishes the job fires
     * this AND On Asset Generated. Wire it to idempotent work only (a label, a
     * progress ring) and use the stage pins for anything that must happen once.
     */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnUpdated;

    /** Waiting for the GPU to render the candidate images. Fires on every snapshot. */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnQueuedForImages;

    /** Rendering the candidate images. Fires on every snapshot; Job.Progress ticks. */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnGeneratingImages;

    /** Candidates are ready to pick. Fires once per entry (a Regenerate re-enters). */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnAwaitingImageSelection;

    /** A candidate was picked; waiting for the GPU again. Fires on every snapshot. */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnQueuedForAsset;

    /** Reconstructing the 3D asset. Fires on every snapshot; Job.Progress restarts. */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnGeneratingAsset;

    /** The asset is in the datalake. Fires once. Spawn it from JobHandle. */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnAssetGenerated;

    /**
     * The job failed; Job.Error is set. Fires once per failure.
     *
     * Also the path for a submit that never produced a job at all — in that case
     * Job.Error carries the reason, JobId is empty and JobHandle is null.
     */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnFailed;

    /** The job was cancelled, by this client or another. Fires once. */
    UPROPERTY(BlueprintAssignable)
    FLoomaFollowGenerationEvent OnCancelled;

    /**
     * Follow a job you already have a handle for.
     *
     * Race-free on an in-flight job: the handle replays its cached state on the next
     * tick, so a job that is already Done still fires On Asset Generated here.
     */
    UFUNCTION(BlueprintCallable,
        meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
            DisplayName = "Follow Generation"),
        Category = "Looma|Generation")
    static ULoomaFollowGenerationAction* FollowGeneration(UObject* WorldContextObject,
        ULoomaGenerationHandle* JobHandle);

    /**
     * Submit a text->3D job (POST /generate) and follow it through every stage.
     *
     * @param Prompt             The text prompt to generate from.
     * @param SuggestedTransform Where an instance of the result is suggested to go
     *                           (sent only if bSuggestTransform).
     * @param bSuggestTransform  Attach SuggestedTransform to the job as advisory
     *                           metadata. Nothing spawns as a result — read it back
     *                           from Job.SuggestedTransform and place the model with
     *                           `Spawn Synced Asset` when you want it.
     * @param NImages            Candidate images to render (clamped 1..8).
     */
    UFUNCTION(BlueprintCallable,
        meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
            DisplayName = "Submit Generation And Follow"),
        Category = "Looma|Generation")
    static ULoomaFollowGenerationAction* SubmitGenerationAndFollow(UObject* WorldContextObject,
        const FString& Prompt, FTransform SuggestedTransform, bool bSuggestTransform = false,
        int32 NImages = 3);

    virtual void Activate() override;

private:
    UFUNCTION()
    void HandleSubmitted(const FString& InJobId, ULoomaGenerationHandle* InHandle);

    UFUNCTION()
    void HandleSubmitFailed(const FString& Error);

    UFUNCTION()
    void HandleUpdated(const FLoomaGenerationJob& Job);

    UFUNCTION()
    void HandleQueueUpdate(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* InHandle, int32 QueuePosition, ELoomaQueuePhase Phase);

    UFUNCTION()
    void HandleGeneratingImages(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* InHandle, float JobProgress, const FString& JobPrompt,
        const FString& JobEnhancedPrompt);

    UFUNCTION()
    void HandleAwaitingImageSelection(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* InHandle, const TArray<FLoomaGeneratedImage>& JobImages);

    UFUNCTION()
    void HandleGeneratingAsset(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* InHandle, const FLoomaGeneratedImage& SelectedImage, float JobProgress);

    UFUNCTION()
    void HandleAssetGenerated(const FString& InJobId, const FLoomaGenerationJob& Job,
        ULoomaGenerationHandle* InHandle, const FString& AssetId);

    UFUNCTION()
    void HandleFailed(const FLoomaGenerationJob& Job);

    UFUNCTION()
    void HandleCancelled(const FLoomaGenerationJob& Job);

    /** Bind to the handle we ended up with, whichever entry point got us here. */
    void BindToHandle(ULoomaGenerationHandle* InHandle);

    /** Broadcast a synthetic failure (no job on the wire) and stop. */
    void FailWith(const FString& Error);

    /** Drop every binding, then release the node. */
    void Finish();

    TWeakObjectPtr<UObject> WorldContextObject;

    UPROPERTY()
    TObjectPtr<ULoomaGenerationHandle> Handle;

    /** Held only so the submit action survives until its HTTP round-trip lands. */
    UPROPERTY()
    TObjectPtr<ULoomaSubmitGenerationAction> SubmitAction;

    FString Prompt;
    FTransform SuggestedTransform;
    bool bSuggestTransform = false;
    bool bSubmitFirst = false;
    int32 NImages = 3;
};
