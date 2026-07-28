#include "LoomaGenerationHandle.h"

#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"

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
        Subsystem->SelectImage(JobId, ImageId);
    }
}

void ULoomaGenerationHandle::Regenerate(const FString& Prompt, int32 NImages)
{
    if (ULoomaSceneSyncSubsystem* Subsystem = GetSubsystem())
    {
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

    OnUpdated.Broadcast(Job);
    if (Job.State == Previous)
    {
        return;
    }
    switch (Job.State)
    {
    case ELoomaJobState::AwaitingImage:
        OnImagesReady.Broadcast(Job);
        break;
    case ELoomaJobState::Done:
        OnDone.Broadcast(Job);
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

void ULoomaGenerationHandle::SeedTransform(const FTransform& InTransform)
{
    Job.SuggestedTransform = InTransform;
    Job.bHasSuggestedTransform = true;
}

ULoomaSceneSyncSubsystem* ULoomaGenerationHandle::GetSubsystem() const
{
    return Cast<ULoomaSceneSyncSubsystem>(GetOuter());
}
