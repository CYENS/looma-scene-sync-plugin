#include "LoomaGenerationListenerComponent.h"

#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "LoomaSceneSyncSubsystem.h"

ULoomaGenerationListenerComponent::ULoomaGenerationListenerComponent()
{
    // Purely event-driven: everything arrives through the handle's delegates.
    PrimaryComponentTick.bCanEverTick = false;
}

void ULoomaGenerationListenerComponent::Watch(ULoomaGenerationHandle* Handle)
{
    if (WatchedHandle == Handle)
    {
        return;
    }

    UnbindFrom(WatchedHandle);
    WatchedHandle = Handle;
    BindTo(WatchedHandle);
}

ULoomaGenerationHandle* ULoomaGenerationListenerComponent::WatchJobId(const FString& JobId)
{
    if (JobId.IsEmpty())
    {
        return nullptr;
    }

    ULoomaSceneSyncSubsystem* Subsystem = GetSubsystem();
    ULoomaGenerationHandle* Handle = Subsystem ? Subsystem->GetGenerationHandle(JobId) : nullptr;
    Watch(Handle);
    return Handle;
}

void ULoomaGenerationListenerComponent::StopWatching()
{
    Watch(nullptr);
}

bool ULoomaGenerationListenerComponent::IsWatching() const
{
    return WatchedHandle != nullptr;
}

FLoomaGenerationJob ULoomaGenerationListenerComponent::GetJob() const
{
    return WatchedHandle ? WatchedHandle->Job : FLoomaGenerationJob();
}

void ULoomaGenerationListenerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Handles live as long as the subsystem, so they outlive this actor. Without
    // this the delegate would keep a stale binding to a destroyed component.
    UnbindFrom(WatchedHandle);
    WatchedHandle = nullptr;

    Super::EndPlay(EndPlayReason);
}

void ULoomaGenerationListenerComponent::BindTo(ULoomaGenerationHandle* Handle)
{
    if (!Handle)
    {
        return;
    }

    Handle->OnUpdated.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleUpdated);
    Handle->OnQueueUpdate.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleQueueUpdate);
    Handle->OnGeneratingImagesUpdate.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleGeneratingImages);
    Handle->OnAwaitingImageSelection.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleAwaitingImageSelection);
    Handle->OnGeneratingAssetUpdate.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleGeneratingAsset);
    Handle->OnAssetGenerated.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleAssetGenerated);
    Handle->OnFailed.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleFailed);
    Handle->OnCancelled.AddDynamic(this, &ULoomaGenerationListenerComponent::HandleCancelled);
}

void ULoomaGenerationListenerComponent::UnbindFrom(ULoomaGenerationHandle* Handle)
{
    if (!Handle)
    {
        return;
    }

    Handle->OnUpdated.RemoveAll(this);
    Handle->OnQueueUpdate.RemoveAll(this);
    Handle->OnGeneratingImagesUpdate.RemoveAll(this);
    Handle->OnAwaitingImageSelection.RemoveAll(this);
    Handle->OnGeneratingAssetUpdate.RemoveAll(this);
    Handle->OnAssetGenerated.RemoveAll(this);
    Handle->OnFailed.RemoveAll(this);
    Handle->OnCancelled.RemoveAll(this);
}

ULoomaSceneSyncSubsystem* ULoomaGenerationListenerComponent::GetSubsystem() const
{
    const UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(this);
    return GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
}

void ULoomaGenerationListenerComponent::HandleQueueUpdate(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* JobHandle, int32 QueuePosition, ELoomaQueuePhase Phase)
{
    OnQueueUpdate.Broadcast(InJobId, Job, JobHandle, QueuePosition, Phase);
}

void ULoomaGenerationListenerComponent::HandleGeneratingImages(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* JobHandle, float JobProgress, const FString& JobPrompt, const FString& JobEnhancedPrompt)
{
    OnGeneratingImagesUpdate.Broadcast(InJobId, Job, JobHandle, JobProgress, JobPrompt, JobEnhancedPrompt);
}

void ULoomaGenerationListenerComponent::HandleAwaitingImageSelection(const FString& InJobId,
    const FLoomaGenerationJob& Job, ULoomaGenerationHandle* JobHandle, const TArray<FLoomaGeneratedImage>& JobImages)
{
    OnAwaitingImageSelection.Broadcast(InJobId, Job, JobHandle, JobImages);
}

void ULoomaGenerationListenerComponent::HandleGeneratingAsset(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* JobHandle, const FLoomaGeneratedImage& SelectedImage, float JobProgress)
{
    OnGeneratingAssetUpdate.Broadcast(InJobId, Job, JobHandle, SelectedImage, JobProgress);
}

void ULoomaGenerationListenerComponent::HandleAssetGenerated(const FString& InJobId, const FLoomaGenerationJob& Job,
    ULoomaGenerationHandle* JobHandle, const FString& AssetId)
{
    OnAssetGenerated.Broadcast(InJobId, Job, JobHandle, AssetId);
}

void ULoomaGenerationListenerComponent::HandleUpdated(const FLoomaGenerationJob& Job)
{
    OnUpdated.Broadcast(Job.JobId, Job, WatchedHandle);
}

void ULoomaGenerationListenerComponent::HandleFailed(const FLoomaGenerationJob& Job)
{
    OnFailed.Broadcast(Job.JobId, Job, WatchedHandle, Job.Error);
}

void ULoomaGenerationListenerComponent::HandleCancelled(const FLoomaGenerationJob& Job)
{
    OnCancelled.Broadcast(Job.JobId, Job, WatchedHandle);
}
