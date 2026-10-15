#include "LoomaGenerationLibrary.h"
#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "LoomaGenerationHandle.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaSubmitGenerationAction.h"
#include "LoomaSyncedActor.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/LoomaGenerationTestObserver.h"
#include "UObject/StrongObjectPtr.h"

/**
 * A job's `meta` end to end, against a real backend: PIE on a blank map, the
 * subsystem connected for real, `Submit Generation` with a meta built by
 * `Make Generation Meta`, and the job followed to Done over the socket. With a
 * scene named, one instance is then placed in it, so a web client on the same
 * backend can select it and show the meta in its Details panel.
 *
 * Opt-in, because it submits a real job: pass -LoomaLiveBackend=<url> naming a
 * backend that keeps a job's meta (looma-xr-asset-demo PR #241 or later). Point it
 * at a worktree backend, ideally in mock mode, never the everyday one:
 *
 *   UnrealEditor-Cmd.exe <host.uproject> -unattended -nullrhi -nosound
 *     -LoomaLiveBackend=http://127.0.0.1:8002 -LoomaLiveScene=demo-studio-floor
 *     -ExecCmds="Automation RunTests Looma.Generation.Meta.Live; Quit"
 */

namespace
{
    struct FLoomaMetaLiveState
    {
        FString PreviousBackendUrl;
        FString Scene;
        FString Environment;
        FString MetaJson;
        TStrongObjectPtr<ULoomaGenerationTestObserver> Observer{NewObject<ULoomaGenerationTestObserver>()};
        TWeakObjectPtr<ULoomaSceneSyncSubsystem> Sync;
        TWeakObjectPtr<UWorld> World;
        double Deadline = 0.0;
        bool bPicked = false;
    };

    ULoomaSceneSyncSubsystem* PieSubsystem(UWorld*& OutWorld)
    {
        OutWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
        UGameInstance* GameInstance = OutWorld ? OutWorld->GetGameInstance() : nullptr;
        return GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
    }

    const TCHAR* StateName(ELoomaJobState State)
    {
        switch (State)
        {
        case ELoomaJobState::Queued:        return TEXT("queued");
        case ELoomaJobState::Running:       return TEXT("running");
        case ELoomaJobState::AwaitingImage: return TEXT("awaiting_image");
        case ELoomaJobState::Done:          return TEXT("done");
        case ELoomaJobState::Failed:        return TEXT("failed");
        case ELoomaJobState::Cancelled:     return TEXT("cancelled");
        default:                            return TEXT("unknown");
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGenerationMetaLiveTest,
    "Looma.Generation.Meta.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaGenerationMetaLiveTest::RunTest(const FString& Parameters)
{
    FString BackendUrl;
    if (!FParse::Value(FCommandLine::Get(), TEXT("LoomaLiveBackend="), BackendUrl) || BackendUrl.IsEmpty())
    {
        AddInfo(TEXT("Skipped: pass -LoomaLiveBackend=<url> naming a backend that keeps a job's meta. It submits one real generation job there."));
        return true;
    }

    const TSharedRef<FLoomaMetaLiveState> State = MakeShared<FLoomaMetaLiveState>();
    FParse::Value(FCommandLine::Get(), TEXT("LoomaLiveScene="), State->Scene);
    ULoomaSceneSyncSettings* Settings = GetMutableDefault<ULoomaSceneSyncSettings>();
    State->PreviousBackendUrl = Settings->BackendUrl;
    Settings->BackendUrl = BackendUrl;
    AddInfo(FString::Printf(TEXT("Backend: %s"), *BackendUrl));

    // A blank map, so nothing in the host project's own level submits a job of its own.
    FAutomationEditorCommonUtils::CreateNewMap();
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));

    State->Deadline = FPlatformTime::Seconds() + 30.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        UWorld* World = nullptr;
        ULoomaSceneSyncSubsystem* Sync = PieSubsystem(World);
        if (!Sync || !Sync->IsSyncConnected())
        {
            if (FPlatformTime::Seconds() > State->Deadline)
            {
                AddError(TEXT("PIE never connected to the backend"));
                return true;
            }
            return false;
        }
        State->Sync = Sync;
        State->World = World;
        Sync->OnGenerationJobUpdated.AddDynamic(State->Observer.Get(), &ULoomaGenerationTestObserver::ObserveJob);
        if (!State->Scene.IsEmpty())
        {
            Sync->OpenSceneByNameOrId(State->Scene);
        }
        State->Deadline = FPlatformTime::Seconds() + 15.0;
        return true;
    }));

    // The hub places nothing until the client has a scene open.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        ULoomaSceneSyncSubsystem* Sync = State->Sync.Get();
        if (State->Scene.IsEmpty() || !Sync)
        {
            return true;
        }
        if (Sync->GetActiveSceneId().IsEmpty())
        {
            if (FPlatformTime::Seconds() > State->Deadline)
            {
                AddError(FString::Printf(TEXT("Scene %s never opened"), *State->Scene));
                return true;
            }
            return false;
        }
        AddInfo(FString::Printf(TEXT("Scene open: %s"), *Sync->GetActiveSceneId()));
        return true;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        UWorld* World = State->World.Get();
        if (!World || !State->Sync.IsValid())
        {
            return true;
        }

        // The environment as the viewer would tag it: the level it was asked from.
        State->Environment = UWorld::RemovePIEPrefix(World->GetMapName());
        TMap<FString, FString> Values;
        Values.Add(TEXT("unreal.environment"), State->Environment);
        Values.Add(TEXT("unreal.check.test"), TEXT("Looma.Generation.Meta.Live"));
        TestTrue(TEXT("the meta builds"), ULoomaGenerationLibrary::MakeGenerationMeta(Values, State->MetaJson));
        AddInfo(FString::Printf(TEXT("Submitting with MetaJson %s"), *State->MetaJson));

        ULoomaSubmitGenerationAction* Action = ULoomaSubmitGenerationAction::SubmitGeneration(
            World, TEXT("a small wooden stool"), FTransform::Identity, false, 1, State->MetaJson);
        Action->OnSubmitted.AddDynamic(State->Observer.Get(), &ULoomaGenerationTestObserver::ObserveSubmitted);
        Action->OnFailed.AddDynamic(State->Observer.Get(), &ULoomaGenerationTestObserver::ObserveError);
        Action->Activate();
        State->Deadline = FPlatformTime::Seconds() + 180.0;
        return true;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        ULoomaGenerationTestObserver* Observer = State->Observer.Get();
        ULoomaSceneSyncSubsystem* Sync = State->Sync.Get();
        if (!Observer->Errors.IsEmpty() || !Sync)
        {
            AddError(FString::Printf(TEXT("Submit failed: %s"),
                Observer->Errors.IsEmpty() ? TEXT("PIE ended") : *Observer->Errors[0]));
            return true;
        }
        FLoomaGenerationJob Job;
        const bool bKnown = !Observer->SubmittedJobId.IsEmpty() && Sync->GetGenerationJob(Observer->SubmittedJobId, Job);
        // Some providers offer even a single candidate (the mock does). Pick it, as a
        // Blueprint would, and keep waiting.
        if (bKnown && Job.State == ELoomaJobState::AwaitingImage && !State->bPicked
            && Observer->Handle && Job.Images.Num() > 0)
        {
            AddInfo(FString::Printf(TEXT("Picking candidate %s"), *Job.Images[0].Id));
            Observer->Handle->SelectImage(Job.Images[0].Id);
            State->bPicked = true;
        }
        const bool bFinished = bKnown && (Job.State == ELoomaJobState::Done
            || Job.State == ELoomaJobState::Failed || Job.State == ELoomaJobState::Cancelled);
        if (!bFinished)
        {
            if (FPlatformTime::Seconds() > State->Deadline)
            {
                AddError(FString::Printf(TEXT("Job %s did not finish in time"), *Observer->SubmittedJobId));
                return true;
            }
            return false;
        }

        AddInfo(FString::Printf(TEXT("Job %s finished, asset %s"), *Job.JobId, *Job.AssetId));
        TestEqual(TEXT("the job is done"), Job.State, ELoomaJobState::Done);
        TestTrue(TEXT("the finished job has its meta"), Job.bHasMeta);
        TestEqual(TEXT("exactly as sent"), Job.MetaJson, State->MetaJson);

        FString Value;
        TestTrue(TEXT("the dotted path finds the environment"),
            ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal.environment"), Value));
        TestEqual(TEXT("the environment it was asked from"), Value, State->Environment);
        TestTrue(TEXT("and the nested value"),
            ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal.check.test"), Value));
        TestEqual(TEXT("unchanged"), Value, FString(TEXT("Looma.Generation.Meta.Live")));

        // Hub-wide. The backend reports a job before it has stored the meta, so its
        // earliest events can lack it. After that, and after our own POST answered,
        // every event has it: the subsystem never lets a known meta go.
        FString Sequence;
        int32 FirstWithMeta = INDEX_NONE;
        int32 Seen = 0;
        for (int32 Index = 0; Index < Observer->Jobs.Num(); ++Index)
        {
            const FLoomaGenerationJob& Event = Observer->Jobs[Index];
            if (Event.JobId != Job.JobId)
            {
                continue;
            }
            Sequence += FString::Printf(TEXT("%s%s%s%s"), Seen > 0 ? TEXT(", ") : TEXT(""),
                Index == Observer->JobsAtSubmitted ? TEXT("| POST answered | ") : TEXT(""),
                StateName(Event.State), Event.bHasMeta ? TEXT(" +meta") : TEXT(" -"));
            if (Event.bHasMeta && FirstWithMeta == INDEX_NONE)
            {
                FirstWithMeta = Seen;
            }
            if (FirstWithMeta != INDEX_NONE)
            {
                TestEqual(FString::Printf(TEXT("hub-wide event %d, after one had it, still has it"), Seen),
                    Event.MetaJson, State->MetaJson);
            }
            if (Index >= Observer->JobsAtSubmitted)
            {
                TestEqual(FString::Printf(TEXT("hub-wide event %d, after the POST answered, has it"), Seen),
                    Event.MetaJson, State->MetaJson);
            }
            ++Seen;
        }
        AddInfo(FString::Printf(TEXT("Hub-wide, %d events: %s"), Seen, *Sequence));
        TestTrue(TEXT("one hub-wide event carried it"), FirstWithMeta != INDEX_NONE);

        // The handle `Submit Generation` gave us: never without it.
        TestTrue(TEXT("the handle reported the job"), Observer->HandleJobs.Num() > 0);
        AddInfo(FString::Printf(TEXT("Handle: %d events, all checked for the meta"), Observer->HandleJobs.Num()));
        for (int32 Index = 0; Index < Observer->HandleJobs.Num(); ++Index)
        {
            TestEqual(FString::Printf(TEXT("handle event %d carries the meta"), Index),
                Observer->HandleJobs[Index].MetaJson, State->MetaJson);
        }

        // Place it, so a web client on this backend can select it and read the meta.
        if (Job.State == ELoomaJobState::Done && Observer->Handle && !State->Scene.IsEmpty())
        {
            const ALoomaSyncedActor* Placed = Observer->Handle->SpawnSyncedAsset(
                TEXT("Generation meta live check"), FTransform(FVector(0.0, 0.0, 0.0)));
            TestNotNull(TEXT("an instance is placed"), Placed);
            if (Placed)
            {
                AddInfo(FString::Printf(TEXT("Placed node %s (asset %s, job %s) in scene %s"),
                    *Placed->Id, *Placed->AssetId, *Placed->JobId, *Sync->GetActiveSceneId()));
            }
        }
        return true;
    }));

    // Long enough for the spawn to reach the hub before the socket closes.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([State]()
    {
        GetMutableDefault<ULoomaSceneSyncSettings>()->BackendUrl = State->PreviousBackendUrl;
        return true;
    }));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
