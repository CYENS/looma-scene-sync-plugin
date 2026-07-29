#include "LoomaGenerationHandle.h"

#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaGeneration, Log, All);

bool ULoomaGenerationHandle::IsFinished() const
{
    return Job.State == ELoomaJobState::Done
        || Job.State == ELoomaJobState::Failed
        || Job.State == ELoomaJobState::Cancelled;
}

void ULoomaGenerationHandle::SelectImage(const FString& ImageId)
{
    if (ULoomaSceneSyncSubsystem* Subsystem = GetSubsystem())
    {
        // Assume the pick lands: the backend re-queues the job and broadcasts that
        // BEFORE it records which image was chosen, so the first snapshot back would
        // otherwise look like the image stage. Corrected either way by the next one.
        bImageSelected = true;
        Subsystem->SelectImage(JobId, ImageId);
    }
}

void ULoomaGenerationHandle::Regenerate(const FString& Prompt, int32 NImages)
{
    if (ULoomaSceneSyncSubsystem* Subsystem = GetSubsystem())
    {
        // Back to the image stage, whatever pick the backend still has on record.
        bImageSelected = false;
        Subsystem->RegenerateImages(JobId, Prompt, NImages);
    }
}

void ULoomaGenerationHandle::Cancel()
{
    if (ULoomaSceneSyncSubsystem* Subsystem = GetSubsystem())
    {
        Subsystem->CancelGeneration(JobId);
    }
}

ALoomaSyncedActor* ULoomaGenerationHandle::SpawnSyncedAsset(const FString& Name, const FTransform& Transform)
{
    if (Job.AssetId.IsEmpty())
    {
        UE_LOG(LogLoomaGeneration, Warning,
            TEXT("SpawnSyncedAsset: job %s has no asset yet (state %d) — wait for OnAssetGenerated."),
            *JobId, static_cast<int32>(Job.State));
        return nullptr;
    }

    ULoomaSceneSyncSubsystem* Subsystem = GetSubsystem();
    return Subsystem ? Subsystem->SpawnSyncedAsset(Job.AssetId, Name, Transform, JobId) : nullptr;
}

ALoomaSyncedActor* ULoomaGenerationHandle::FindSpawnedActor() const
{
    ULoomaSceneSyncSubsystem* Subsystem = GetSubsystem();
    return Subsystem ? Subsystem->FindSyncedActorByJobId(JobId) : nullptr;
}

void ULoomaGenerationHandle::Apply(const FLoomaGenerationJob& InJob)
{
    // Compare against the state we last saw so the state events fire on the
    // transition only — the WS event and the REST hydrate can both deliver the
    // same state, and Regenerate legitimately re-enters AwaitingImage.
    const ELoomaJobState Previous = bHasApplied ? Job.State : ELoomaJobState::Unknown;

    // A job's suggested pose is set once, at submit, and never cleared — but the
    // backend announces the job before it has stored it, so an early event can
    // arrive without one. Keep what we already know instead of downgrading
    // Job.bHasSuggestedTransform back to false.
    const bool bKeepKnownTransform = !InJob.bHasSuggestedTransform && Job.bHasSuggestedTransform;
    const FTransform KnownTransform = Job.SuggestedTransform;

    Job = InJob;
    JobId = InJob.JobId;
    bHasApplied = true;

    if (bKeepKnownTransform)
    {
        Job.SuggestedTransform = KnownTransform;
        Job.bHasSuggestedTransform = true;
    }

    UpdateImageSelected(Previous);

    OnUpdated.Broadcast(Job);

    // Stage events that track a value as it moves — queue position, progress — fire
    // on every snapshot, which is what makes them useful for a progress ring.
    switch (Job.State)
    {
    case ELoomaJobState::Queued:
        OnQueueUpdate.Broadcast(JobId, Job, this, Job.QueuePosition, GetPhase());
        break;
    case ELoomaJobState::Running:
        if (GetPhase() == ELoomaQueuePhase::Asset)
        {
            OnGeneratingAssetUpdate.Broadcast(JobId, Job, this, FindSelectedImage(), Job.Progress);
        }
        else
        {
            OnGeneratingImagesUpdate.Broadcast(JobId, Job, this, Job.Progress, Job.Prompt, Job.EnhancedPrompt);
        }
        break;
    default:
        break;
    }

    if (Job.State == Previous)
    {
        return;
    }
    switch (Job.State)
    {
    case ELoomaJobState::AwaitingImage:
        OnImagesReady.Broadcast(Job);
        OnAwaitingImageSelection.Broadcast(JobId, Job, this, Job.Images);
        break;
    case ELoomaJobState::Done:
        OnDone.Broadcast(Job);
        OnAssetGenerated.Broadcast(JobId, Job, this, Job.AssetId);
        break;
    case ELoomaJobState::Failed:
        OnFailed.Broadcast(Job);
        break;
    case ELoomaJobState::Cancelled:
        OnCancelled.Broadcast(Job);
        break;
    default:
        break;
    }
}

void ULoomaGenerationHandle::UpdateImageSelected(ELoomaJobState PreviousState)
{
    // Parked on the candidates: by definition nothing is picked yet. A Regenerate
    // comes back through here, which is what un-sticks the flag after a re-roll.
    if (Job.State == ELoomaJobState::AwaitingImage)
    {
        bImageSelected = false;
        return;
    }

    // Failed -> Queued can only be a regenerate (the backend allows a re-roll from
    // AwaitingImage or Failed only), so we are rendering images again — regardless
    // of the pick the backend still has on record from the attempt that failed.
    if (PreviousState == ELoomaJobState::Failed && Job.State == ELoomaJobState::Queued)
    {
        bImageSelected = false;
        return;
    }

    if (!Job.SelectedImageUrl.IsEmpty())
    {
        bImageSelected = true;
    }
}

ELoomaQueuePhase ULoomaGenerationHandle::GetPhase() const
{
    return bImageSelected ? ELoomaQueuePhase::Asset : ELoomaQueuePhase::Images;
}

FLoomaGeneratedImage ULoomaGenerationHandle::FindSelectedImage() const
{
    if (!Job.SelectedImageUrl.IsEmpty())
    {
        for (const FLoomaGeneratedImage& Image : Job.Images)
        {
            if (Image.Url == Job.SelectedImageUrl)
            {
                return Image;
            }
        }
    }

    // No manifest to match against (or nothing picked yet, on the racy first
    // post-selection snapshot). Hand back what we do know.
    FLoomaGeneratedImage Selected;
    Selected.Url = Job.SelectedImageUrl;
    return Selected;
}

void ULoomaGenerationHandle::SeedTransform(const FTransform& InTransform)
{
    Job.SuggestedTransform = InTransform;
    Job.bHasSuggestedTransform = true;
}

ULoomaSceneSyncSubsystem* ULoomaGenerationHandle::GetSubsystem() const
{
    return Cast<ULoomaSceneSyncSubsystem>(GetOuter());
}
