#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "LoomaSubmitGenerationAction.generated.h"

class ULoomaGenerationHandle;
class ULoomaSceneSyncSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FLoomaSubmitSucceeded, const FString&, JobId,
    ULoomaGenerationHandle*, Handle);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaSubmitFailed, const FString&, Error);

/**
 * Async Blueprint node: submit a text->3D generation job (POST /generate).
 *
 * OnSubmitted fires with the new job_id and a ULoomaGenerationHandle for it —
 * bind the handle's events to follow just this job. Either family works: the raw
 * state events (OnUpdated / OnImagesReady / OnDone / OnFailed) or the per-stage
 * ones (OnQueueUpdate / OnGeneratingImagesUpdate / OnAwaitingImageSelection /
 * OnGeneratingAssetUpdate / OnAssetGenerated), which break the values that matter
 * at each stage out onto their own pins. The subsystem's OnGenerationJob* events
 * remain available if you'd rather see every job on the hub and filter by id.
 */
UCLASS()
class LOOMASCENESYNC_API ULoomaSubmitGenerationAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FLoomaSubmitSucceeded OnSubmitted;

    UPROPERTY(BlueprintAssignable)
    FLoomaSubmitFailed OnFailed;

    /**
     * @param Prompt            The text prompt to generate from.
     * @param SuggestedTransform Where an instance of the result is suggested to go
     *                           (sent only if bSuggestTransform).
     * @param bSuggestTransform  Attach SuggestedTransform to the job as advisory
     *                           metadata. Nothing spawns as a result — read it back
     *                           from the job and place the model with
     *                           `Spawn Synced Asset` when you want it.
     * @param NImages            Candidate images to render (clamped 1..8).
     */
    UFUNCTION(BlueprintCallable,
        meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
        Category = "Looma|Generation")
    static ULoomaSubmitGenerationAction* SubmitGeneration(UObject* WorldContextObject, const FString& Prompt,
        FTransform SuggestedTransform, bool bSuggestTransform = false, int32 NImages = 3);

    virtual void Activate() override;

private:
    void OnResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);

    TWeakObjectPtr<UObject> WorldContextObject;
    TWeakObjectPtr<ULoomaSceneSyncSubsystem> Subsystem;
    FString Prompt;
    FTransform SuggestedTransform;
    bool bSuggestTransform = false;
    int32 NImages = 3;
};
