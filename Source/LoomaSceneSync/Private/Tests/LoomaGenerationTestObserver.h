#pragma once

#include "CoreMinimal.h"
#include "LoomaGenerationHandle.h"
#include "LoomaGenerationTypes.h"
#include "UObject/Object.h"
#include "LoomaGenerationTestObserver.generated.h"

// Records what the generation delegates deliver. Dynamic delegates need reflected
// callbacks; like ULoomaPresenceTestObserver, only the tests make one.
UCLASS(Transient, NotBlueprintable)
class ULoomaGenerationTestObserver : public UObject
{
    GENERATED_BODY()

public:
    TArray<FLoomaGenerationJob> Jobs;
    TArray<FString> Errors;

    /** What `Submit Generation` handed back, and every event of that handle since. */
    FString SubmittedJobId;
    UPROPERTY()
    TObjectPtr<ULoomaGenerationHandle> Handle;
    TArray<FLoomaGenerationJob> HandleJobs;
    /** How many hub-wide events had arrived when the submit's response did. */
    int32 JobsAtSubmitted = INDEX_NONE;

    UFUNCTION()
    void ObserveJob(const FLoomaGenerationJob& Job)
    {
        Jobs.Add(Job);
    }

    UFUNCTION()
    void ObserveError(const FString& Error)
    {
        Errors.Add(Error);
    }

    UFUNCTION()
    void ObserveSubmitted(const FString& JobId, ULoomaGenerationHandle* InHandle)
    {
        SubmittedJobId = JobId;
        Handle = InHandle;
        JobsAtSubmitted = Jobs.Num();
        if (InHandle)
        {
            // Bound as the node hands it over, as a Blueprint would, so the handle's
            // next-tick replay is the first thing it reports.
            InHandle->OnUpdated.AddDynamic(this, &ULoomaGenerationTestObserver::ObserveHandleJob);
        }
    }

    UFUNCTION()
    void ObserveHandleJob(const FLoomaGenerationJob& Job)
    {
        HandleJobs.Add(Job);
    }
};
