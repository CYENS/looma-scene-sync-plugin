#include "LoomaSceneSyncSubsystem.h"
#include "LoomaTerrainActor.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaWireConvert.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProceduralMeshComponent.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

/**
 * The ground followed live, against a real backend: PIE on a blank map, the subsystem
 * connected for real, a scene with a terrain opened, and then a change made somewhere
 * else (the web, or a script speaking for it) awaited until it is drawn here and its
 * collision stands where the new ground is.
 *
 * Opt-in. Name a scene whose ground has a terrain or an Ocean, a point on it in wire
 * metres from the ground's host, and the height that point is about to be sculpted to:
 *
 *   UnrealEditor-Cmd.exe <host.uproject> -unattended -nullrhi -nosound
 *     -LoomaLiveBackend=http://127.0.0.1:8001 -LoomaLiveScene=<scene id>
 *     -LoomaTerrainProbe=-25,-25 -LoomaTerrainExpect=10
 *     -ExecCmds="Automation RunTests Looma.Terrain.Live; Quit"
 *
 * The test logs LOOMA_TERRAIN_READY once the scene's ground is drawn; make the change
 * after that. Point it at a worktree backend, never the everyday one.
 */
namespace LoomaTerrainLive
{
struct FTerrainLiveState
{
    FString PreviousBackendUrl;
    FString Scene;
    FVector2D Probe = FVector2D::ZeroVector;
    float Expect = 0.0f;
    TWeakObjectPtr<ULoomaSceneSyncSubsystem> Sync;
    TWeakObjectPtr<UWorld> World;
    double Deadline = 0.0;
    double ReadyAt = 0.0;
};

ULoomaSceneSyncSubsystem* TerrainPieSubsystem(UWorld*& OutWorld)
{
    OutWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
    UGameInstance* GameInstance = OutWorld ? OutWorld->GetGameInstance() : nullptr;
    return GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
}

/** The world point above the probe, at the ground's host's height. */
bool TerrainProbePoint(const FTerrainLiveState& State, FVector& Out)
{
    ULoomaSceneSyncSubsystem* Sync = State.Sync.Get();
    ALoomaTerrainActor* Terrain = Sync ? Sync->GetTerrainActor() : nullptr;
    if (!Terrain || !Terrain->IsShowing())
    {
        return false;
    }
    Out = Terrain->GetActorLocation() + LoomaWirePointToUe(FVector(State.Probe.X, 0.0, State.Probe.Y));
    return true;
}

/** The ground's height at the probe, wire metres above its host, or false with no ground there. */
bool TerrainProbeHeight(const FTerrainLiveState& State, float& Out)
{
    FVector Point;
    float WorldZ = 0.0f;
    ALoomaTerrainActor* Terrain = State.Sync.IsValid() ? State.Sync->GetTerrainActor() : nullptr;
    if (!TerrainProbePoint(State, Point) || !Terrain->GetGroundHeight(Point, WorldZ))
    {
        return false;
    }
    Out = (WorldZ - static_cast<float>(Terrain->GetActorLocation().Z)) / 100.0f;
    return true;
}
} // namespace LoomaTerrainLive

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaTerrainLiveTest, "Looma.Terrain.Live",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaTerrainLiveTest::RunTest(const FString& Parameters)
{
    using namespace LoomaTerrainLive;
    FString BackendUrl;
    const TSharedRef<FTerrainLiveState> State = MakeShared<FTerrainLiveState>();
    FParse::Value(FCommandLine::Get(), TEXT("LoomaLiveScene="), State->Scene);
    if (!FParse::Value(FCommandLine::Get(), TEXT("LoomaLiveBackend="), BackendUrl) || BackendUrl.IsEmpty()
        || State->Scene.IsEmpty())
    {
        AddInfo(TEXT("Skipped: pass -LoomaLiveBackend=<url> and -LoomaLiveScene=<id> naming a scene with a ground."));
        return true;
    }
    FString Probe;
    if (FParse::Value(FCommandLine::Get(), TEXT("LoomaTerrainProbe="), Probe, false))
    {
        FString X;
        FString Z;
        if (Probe.Split(TEXT(","), &X, &Z))
        {
            State->Probe = FVector2D(FCString::Atof(*X), FCString::Atof(*Z));
        }
    }
    FParse::Value(FCommandLine::Get(), TEXT("LoomaTerrainExpect="), State->Expect);

    ULoomaSceneSyncSettings* Settings = GetMutableDefault<ULoomaSceneSyncSettings>();
    State->PreviousBackendUrl = Settings->BackendUrl;
    Settings->BackendUrl = BackendUrl;
    AddInfo(FString::Printf(TEXT("Backend %s, scene %s, probe (%.2f, %.2f) to %.3f m"), *BackendUrl, *State->Scene,
        State->Probe.X, State->Probe.Y, State->Expect));

    FAutomationEditorCommonUtils::CreateNewMap();
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));

    State->Deadline = FPlatformTime::Seconds() + 30.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        UWorld* World = nullptr;
        ULoomaSceneSyncSubsystem* Sync = TerrainPieSubsystem(World);
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
        State->Deadline = FPlatformTime::Seconds() + 30.0;
        return true;
    }));

    // The scene is open and its ground drawn, every tile it lists in place.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        ULoomaSceneSyncSubsystem* Sync = State->Sync.Get();
        ALoomaTerrainActor* Terrain = Sync ? Sync->GetTerrainActor() : nullptr;
        float Height = 0.0f;
        if (!Terrain || !Terrain->IsShowing() || Terrain->GetWaitingTiles() > 0 || !TerrainProbeHeight(*State, Height))
        {
            if (FPlatformTime::Seconds() > State->Deadline)
            {
                AddError(FString::Printf(TEXT("The scene's ground was never drawn (scene open: '%s', actor: %s)"),
                    Sync ? *Sync->GetActiveSceneId() : TEXT(""), Terrain ? TEXT("yes") : TEXT("no")));
                State->Deadline = 0.0;
                return true;
            }
            return false;
        }
        State->ReadyAt = FPlatformTime::Seconds();
        AddInfo(FString::Printf(TEXT("Ground drawn on %s (%s), %d tiles kept, %.3f m at the probe"),
            *Terrain->GetHostId(), Terrain->IsSeaFloor() ? TEXT("sea floor") : TEXT("terrain"),
            Terrain->GetKeptTiles(), Height));
        UE_LOG(LogTemp, Display, TEXT("LOOMA_TERRAIN_READY %s"), *Terrain->GetHostId());
        State->Deadline = FPlatformTime::Seconds() + 120.0;
        return true;
    }));

    // Then the change made elsewhere arrives, and is drawn.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        if (State->ReadyAt <= 0.0)
        {
            return true;
        }
        float Height = 0.0f;
        if (!TerrainProbeHeight(*State, Height) || !FMath::IsNearlyEqual(Height, State->Expect, 0.01f))
        {
            if (FPlatformTime::Seconds() > State->Deadline)
            {
                AddError(FString::Printf(TEXT("The probe never reached %.3f m (last %.3f m)"), State->Expect, Height));
                State->ReadyAt = 0.0;
                return true;
            }
            return false;
        }
        ALoomaTerrainActor* Terrain = State->Sync->GetTerrainActor();
        AddInfo(FString::Printf(TEXT("Drawn at %.3f m, %.1f s after the ground was ready; %d tiles kept"), Height,
            FPlatformTime::Seconds() - State->ReadyAt, Terrain->GetKeptTiles()));
        UE_LOG(LogTemp, Display, TEXT("LOOMA_TERRAIN_CHANGED %.3f"), Height);
        State->Deadline = FPlatformTime::Seconds() + 15.0;
        return true;
    }));

    // And the collision stands on the new ground once it has settled and cooked.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
    {
        if (State->ReadyAt <= 0.0)
        {
            return true; // an earlier step failed and said so
        }
        UWorld* World = State->World.Get();
        FVector Point;
        if (!World || !TerrainProbePoint(*State, Point))
        {
            AddError(TEXT("The play world or its ground went away before the collision could be traced"));
            return true;
        }
        const double ExpectZ = Point.Z + State->Expect * 100.0;
        FHitResult Hit;
        const bool bHit = World->LineTraceSingleByChannel(Hit, FVector(Point.X, Point.Y, ExpectZ + 5000.0),
            FVector(Point.X, Point.Y, ExpectZ - 5000.0), ECC_Visibility);
        if (!bHit || !FMath::IsNearlyEqual(Hit.ImpactPoint.Z, ExpectZ, 2.0))
        {
            if (FPlatformTime::Seconds() > State->Deadline)
            {
                AddError(FString::Printf(TEXT("A trace never met the new ground at Z %.1f (hit: %s, Z %.1f)"), ExpectZ,
                    bHit ? TEXT("yes") : TEXT("no"), bHit ? Hit.ImpactPoint.Z : 0.0));
                return true;
            }
            return false;
        }
        const FString Met = FString::Printf(TEXT("A trace meets the ground's collision at Z %.1f cm (%s)"),
            Hit.ImpactPoint.Z, Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("?"));
        AddInfo(Met);
        UE_LOG(LogTemp, Display, TEXT("LOOMA_TERRAIN_COLLISION %s"), *Met);
        return true;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([State]()
    {
        GetMutableDefault<ULoomaSceneSyncSettings>()->BackendUrl = State->PreviousBackendUrl;
        return true;
    }));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
