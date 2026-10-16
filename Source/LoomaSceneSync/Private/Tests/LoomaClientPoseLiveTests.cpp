#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "LoomaRemotePerson.h"
#include "LoomaSceneSyncSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

/**
 * Where everyone is, end to end against a real backend (HAM-311 part 1): PIE on a
 * blank map connects for real and opens a scene, then waits for somebody else on
 * that scene to say where they are (a web client in VR, or its emulator) and checks
 * a figure is drawn there. Then it says where it is itself, twenty times a second
 * for a few seconds, at a fixed spot a web client on the same scene can look for,
 * and takes itself away again.
 *
 * Opt-in: pass -LoomaLiveBackend=<url> and -LoomaLiveScene=<a scene a guest may
 * read>, and have someone in VR on that scene within LoomaLiveWait seconds (60 by
 * default). It changes nothing in the scene.
 *
 *   UnrealEditor-Cmd.exe <host.uproject> -unattended -nullrhi -nosound
 *     -LoomaLiveBackend=http://127.0.0.1:8003 -LoomaLiveScene=demo-studio-floor
 *     -ExecCmds="Automation RunTests Looma.Presence.ClientPose.Live; Quit"
 */

namespace
{
    struct FLoomaPoseLiveState
    {
        FString PreviousBackendUrl;
        FString Scene;
        TWeakObjectPtr<ULoomaSceneSyncSubsystem> Sync;
        TWeakObjectPtr<UWorld> World;
        double Deadline = 0.0;
        double SendUntil = 0.0;
        double NextSend = 0.0;
        double Wait = 60.0;
    };

    ULoomaSceneSyncSubsystem* PieSync(UWorld*& OutWorld)
    {
        OutWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
        UGameInstance* GameInstance = OutWorld ? OutWorld->GetGameInstance() : nullptr;
        return GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
    }

    // Where this test stands while it says where it is: 3 m along +X from the
    // scene origin, eyes at 1.7 m. On the wire that is (0, 1.7, -3).
    const FTransform LiveHead(FRotator(0.0, 180.0, 0.0), FVector(300.0, 0.0, 170.0));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaClientPoseLiveTest,
    "Looma.Presence.ClientPose.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaClientPoseLiveTest::RunTest(const FString& Parameters)
{
    FString BackendUrl;
    const TSharedRef<FLoomaPoseLiveState> State = MakeShared<FLoomaPoseLiveState>();
    if (!FParse::Value(FCommandLine::Get(), TEXT("LoomaLiveBackend="), BackendUrl) || BackendUrl.IsEmpty()
        || !FParse::Value(FCommandLine::Get(), TEXT("LoomaLiveScene="), State->Scene) || State->Scene.IsEmpty())
    {
        AddInfo(TEXT("Skipped: pass -LoomaLiveBackend=<url> and -LoomaLiveScene=<scene>, with somebody in VR on that scene."));
        return true;
    }
    FParse::Value(FCommandLine::Get(), TEXT("LoomaLiveWait="), State->Wait);
    ULoomaSceneSyncSettings* Settings = GetMutableDefault<ULoomaSceneSyncSettings>();
    State->PreviousBackendUrl = Settings->BackendUrl;
    Settings->BackendUrl = BackendUrl;
    AddInfo(FString::Printf(TEXT("Backend: %s, scene: %s"), *BackendUrl, *State->Scene));

    FAutomationEditorCommonUtils::CreateNewMap();
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));

    State->Deadline = FPlatformTime::Seconds() + 30.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        UWorld* World = nullptr;
        ULoomaSceneSyncSubsystem* Sync = PieSync(World);
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
        Sync->OpenSceneByNameOrId(State->Scene);
        State->Deadline = FPlatformTime::Seconds() + State->Wait;
        return true;
    }));

    // Somebody else says where they are, and a figure is drawn there.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        ULoomaSceneSyncSubsystem* Sync = State->Sync.Get();
        UWorld* World = State->World.Get();
        if (!Sync || !World)
        {
            return true;
        }
        const TArray<FLoomaClientPose> Poses = Sync->GetClientPoses();
        ALoomaRemotePerson* Figure = nullptr;
        for (TActorIterator<ALoomaRemotePerson> It(World); It; ++It)
        {
            Figure = *It;
        }
        if (Poses.Num() == 0 || !Figure)
        {
            if (FPlatformTime::Seconds() > State->Deadline)
            {
                AddError(FString::Printf(TEXT("Nobody on %s said where they were (poses %d, figure %s)"),
                    *State->Scene, Poses.Num(), Figure ? TEXT("yes") : TEXT("no")));
                return true;
            }
            return false;
        }
        const FLoomaClientPose& Pose = Poses[0];
        AddInfo(FString::Printf(TEXT("Heard %s (%s) at %s, %s hand(s)"), *Pose.ClientId, *Pose.Hint,
            *Pose.Head.GetLocation().ToString(),
            Pose.bHasLeftHand && Pose.bHasRightHand ? TEXT("two") : (Pose.bHasLeftHand || Pose.bHasRightHand ? TEXT("one") : TEXT("no"))));
        TestTrue(TEXT("the figure stands near the pose"),
            FVector::Dist(Figure->Head->GetComponentLocation(), Pose.Head.GetLocation()) < 50.0);
        State->SendUntil = FPlatformTime::Seconds() + 6.0;
        return true;
    }));

    // Now say where we are, for a web client on the scene to see.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        ULoomaSceneSyncSubsystem* Sync = State->Sync.Get();
        if (!Sync)
        {
            return true;
        }
        const double Now = FPlatformTime::Seconds();
        if (Now >= State->SendUntil)
        {
            Sync->ClearClientPose();
            return true;
        }
        if (Now >= State->NextSend)
        {
            State->NextSend = Now + 0.05;
            const FTransform Left(FVector(320.0, -25.0, 120.0));
            const FTransform Right(FVector(320.0, 25.0, 140.0));
            Sync->SendClientPose(LiveHead, true, Left, true, Right, TEXT("vr-head"));
        }
        return false;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([State]()
    {
        GetMutableDefault<ULoomaSceneSyncSettings>()->BackendUrl = State->PreviousBackendUrl;
        return true;
    }));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
