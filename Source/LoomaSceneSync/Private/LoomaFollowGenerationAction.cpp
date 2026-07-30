#include "LoomaFollowGenerationAction.h"

#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "LoomaGenerationHandle.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSubmitGenerationAction.h"

ULoomaFollowGenerationAction* ULoomaFollowGenerationAction::FollowGeneration(UObject* WorldContextObject,
    ULoomaGenerationHandle* JobHandle)
{
    ULoomaFollowGenerationAction* Action = NewObject<ULoomaFollowGenerationAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Handle = JobHandle;
    Action->bSubmitFirst = false;
    return Action;
}

ULoomaFollowGenerationAction* ULoomaFollowGenerationAction::SubmitGenerationAndFollow(UObject* WorldContextObject,
    const FString& Prompt, FTransform SuggestedTransform, bool bSuggestTransform, int32 NImages)
{
    ULoomaFollowGenerationAction* Action = NewObject<ULoomaFollowGenerationAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Prompt = Prompt;
    Action->SuggestedTransform = SuggestedTransform;
    Action->bSuggestTransform = bSuggestTransform;
    Action->NImages = NImages;
    Action->bSubmitFirst = true;
    return Action;
}

void ULoomaFollowGenerationAction::Activate()
{
    UGameInstance* GameInstance = WorldContextObject.IsValid()
        ? UGameplayStatics::GetGameInstance(WorldContextObject.Get())
        : nullptr;

    if (!GameInstance)
    {
        FailWith(TEXT("No game instance for the supplied world context"));
        return;
    }

    // The job outlives the node's caller, so the node has to stay alive on its own.
    RegisterWithGameInstance(GameInstance);

    if (bSubmitFirst)
    {
        // Reuse the existing submit node rather than duplicating the POST: it already
        // mints the handle before broadcasting and registers the suggested pose with
        // the subsystem, both of which we would otherwise have to repeat here.
        SubmitAction = ULoomaSubmitGenerationAction::SubmitGeneration(
            WorldContextObject.Get(), Prompt, SuggestedTransform, bSuggestTransform, NImages);
        if (!SubmitAction)
        {
            FailWith(TEXT("Could not create the submit action"));
            return;
        }

        SubmitAction->OnSubmitted.AddDynamic(this, &ULoomaFollowGenerationAction::HandleSubmitted);
        SubmitAction->OnFailed.AddDynamic(this, &ULoomaFollowGenerationAction::HandleSubmitFailed);
        SubmitAction->Activate();
        return;
    }

    if (!Handle)
    {
        FailWith(TEXT("Follow Generation was given a null job handle"));
        return;
    }
    BindToHandle(Handle);
}

void ULoomaFollowGenerationAction::HandleSubmitted(const FString& InJobId, ULoomaGenerationHandle* InHandle)
{
    if (!InHandle)
    {
        FailWith(FString::Printf(TEXT("Job %s was submitted but no handle could be created"), *InJobId));
        return;
    }

    Handle = InHandle;
    BindToHandle(Handle);
}

void ULoomaFollowGenerationAction::HandleSubmitFailed(const FString& Error)
{
    FailWith(Error);
}

void ULoomaFollowGenerationAction::BindToHandle(ULoomaGenerationHandle* InHandle)
{
    if (!InHandle)
    {
        return;
    }

    InHandle->OnUpdated.AddDynamic(this, &ULoomaFollowGenerationAction::HandleUpdated);
    InHandle->OnQueueUpdate.AddDynamic(this, &ULoomaFollowGenerationAction::HandleQueueUpdate);
    InHandle->OnGeneratingImagesUpdate.AddDynamic(this, &ULoomaFollowGenerationAction::HandleGeneratingImages);
    InHandle->OnAwaitingImageSelection.AddDynamic(this, &ULoomaFollowGenerationAction::HandleAwaitingImageSelection);
    InHandle->OnGeneratingAssetUpdate.AddDynamic(this, &ULoomaFollowGenerationAction::HandleGeneratingAsset);
    InHandle->OnAssetGenerated.AddDynamic(this, &ULoomaFollowGenerationAction::HandleAssetGenerated);
    InHandle->OnFailed.AddDynamic(this, &ULoomaFollowGenerationAction::HandleFailed);
    InHandle->OnCancelled.AddDynamic(this, &ULoomaFollowGenerationAction::HandleCancelled);
}

void ULoomaFollowGenerationAction::HandleUpdated(const FLoomaGenerationJob& Job)
{
    OnUpdated.Broadcast(Job, Handle);
}

void ULoomaFollowGenerationAction::HandleQueueUpdate(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* InHandle, int32 QueuePosition, ELoomaQueuePhase Phase)
{
    // Phase is what separates the two queue waits; carrying it as a pin would have
    // meant a non-uniform signature, so it selects the exec pin instead.
    if (Phase == ELoomaQueuePhase::Asset)
    {
        OnQueuedForAsset.Broadcast(Job, InHandle);
    }
    else
    {
        OnQueuedForImages.Broadcast(Job, InHandle);
    }
}

void ULoomaFollowGenerationAction::HandleGeneratingImages(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* InHandle, float JobProgress, const FString& JobPrompt, const FString& JobEnhancedPrompt)
{
    OnGeneratingImages.Broadcast(Job, InHandle);
}

void ULoomaFollowGenerationAction::HandleAwaitingImageSelection(const FString& InJobId,
    const FLoomaGenerationJob& Job, ULoomaGenerationHandle* InHandle, const TArray<FLoomaGeneratedImage>& JobImages)
{
    OnAwaitingImageSelection.Broadcast(Job, InHandle);
}

void ULoomaFollowGenerationAction::HandleGeneratingAsset(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* InHandle, const FLoomaGeneratedImage& SelectedImage, float JobProgress)
{
    OnGeneratingAsset.Broadcast(Job, InHandle);
}

void ULoomaFollowGenerationAction::HandleAssetGenerated(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* InHandle, const FString& AssetId)
{
    OnAssetGenerated.Broadcast(Job, InHandle);
    Finish();
}

void ULoomaFollowGenerationAction::HandleFailed(const FLoomaGenerationJob& Job)
{
    // No Finish() here on purpose: `Regenerate` can take a failed job back to the
    // image stage, and a graph that re-rolls should keep receiving the later stages.
    OnFailed.Broadcast(Job, Handle);
}

void ULoomaFollowGenerationAction::HandleCancelled(const FLoomaGenerationJob& Job)
{
    OnCancelled.Broadcast(Job, Handle);
    Finish();
}

void ULoomaFollowGenerationAction::FailWith(const FString& Error)
{
    FLoomaGenerationJob Job;
    Job.State = ELoomaJobState::Failed;
    Job.Error = Error;
    OnFailed.Broadcast(Job, Handle);
    Finish();
}

void ULoomaFollowGenerationAction::Finish()
{
    // Unbind before releasing: handles live as long as the subsystem, so a leftover
    // binding would outlive this node.
    if (Handle)
    {
        Handle->OnUpdated.RemoveAll(this);
        Handle->OnQueueUpdate.RemoveAll(this);
        Handle->OnGeneratingImagesUpdate.RemoveAll(this);
        Handle->OnAwaitingImageSelection.RemoveAll(this);
        Handle->OnGeneratingAssetUpdate.RemoveAll(this);
        Handle->OnAssetGenerated.RemoveAll(this);
        Handle->OnFailed.RemoveAll(this);
        Handle->OnCancelled.RemoveAll(this);
    }
    if (SubmitAction)
    {
        SubmitAction->OnSubmitted.RemoveAll(this);
        SubmitAction->OnFailed.RemoveAll(this);
        SubmitAction = nullptr;
    }

    SetReadyToDestroy();
}
