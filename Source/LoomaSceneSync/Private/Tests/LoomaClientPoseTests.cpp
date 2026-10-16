#include "LoomaPresenceTypes.h"
#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "LoomaRemotePerson.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

/**
 * Where everyone is (`clientPose`, HAM-311 part 1). Frames go through the same JSON
 * dispatcher as the socket's, into a subsystem that never opened one; with no world
 * nothing is drawn, so these read the table the figures are drawn from.
 */
struct FLoomaClientPoseTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};

    FLoomaClientPoseTestAccess() { Sync->ClientId = TEXT("self"); }

    void Receive(const FString& Json) { Sync->OnRawMessage(Json); }
    void Disconnect() { Sync->ClearPresence(); }
    const FLoomaClientPose* Find(const TCHAR* Id) const { return Sync->ClientPoses.Find(Id); }
    int32 Num() const { return Sync->ClientPoses.Num(); }
};

namespace
{
    const TCHAR* const PoseOfB =
        TEXT("{\"type\":\"clientPose\",\"clientId\":\"B\",\"color\":\"#6047e1\",\"hint\":\"vr-head\",")
        TEXT("\"t\":{\"p\":[1,1.6,-2],\"q\":[0,0,0,1]},")
        TEXT("\"hands\":{\"left\":{\"p\":[0.8,1.3,-1.7],\"q\":[0,0,0,1]},\"right\":\"x\"}}");

    TSharedPtr<FJsonObject> ParseObject(const TCHAR* Json)
    {
        TSharedPtr<FJsonObject> Out;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out);
        return Out;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaClientPoseReceiveTest,
    "Looma.Presence.ClientPose.Receive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaClientPoseReceiveTest::RunTest(const FString& Parameters)
{
    FLoomaClientPoseTestAccess Access;
    Access.Receive(PoseOfB);
    const FLoomaClientPose* B = Access.Find(TEXT("B"));
    if (!TestNotNull(TEXT("B's pose is kept"), B))
    {
        return false;
    }
    // Wire (x, y, z) metres -> UE (-z, x, y) centimetres.
    TestTrue(TEXT("the head is converted to UE space"),
        B->Head.GetLocation().Equals(FVector(200.0, 100.0, 160.0), 0.01));
    TestEqual(TEXT("the hint is kept"), B->Hint, FString(TEXT("vr-head")));
    TestEqual(TEXT("the hub's colour"), B->ColorHex, FString(TEXT("#6047e1")));
    TestTrue(TEXT("the left hand is there"), B->bHasLeftHand);
    TestFalse(TEXT("a malformed right hand is left out"), B->bHasRightHand);
    // A headset looks down the wire's -Z, which is UE's +X: an identity quaternion
    // on the wire is a head looking along +X here.
    TestTrue(TEXT("the head looks down +X"),
        B->Head.GetRotation().GetForwardVector().Equals(FVector::ForwardVector, 1e-4));

    // Ours is never drawn, and `t: null` takes a client away.
    Access.Receive(TEXT("{\"type\":\"clientPose\",\"clientId\":\"self\",\"t\":{\"p\":[0,1,0],\"q\":[0,0,0,1]}}"));
    TestNull(TEXT("our own pose is ignored"), Access.Find(TEXT("self")));
    Access.Receive(TEXT("{\"type\":\"clientPose\",\"clientId\":\"B\",\"color\":\"#6047e1\",\"t\":null}"));
    TestNull(TEXT("t: null clears B"), Access.Find(TEXT("B")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaClientPoseRosterTest,
    "Looma.Presence.ClientPose.Roster",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaClientPoseRosterTest::RunTest(const FString& Parameters)
{
    FLoomaClientPoseTestAccess Access;
    // B is heard from directly, at x = 3 m.
    Access.Receive(TEXT("{\"type\":\"clientPose\",\"clientId\":\"B\",\"color\":\"#6047e1\",")
                   TEXT("\"t\":{\"p\":[3,1.5,0],\"q\":[0,0,0,1]}}"));
    // A roster that lists B with an older pose, C with one, and D with none.
    Access.Receive(TEXT("{\"type\":\"clients\",\"you\":\"self\",\"clients\":[")
        TEXT("{\"id\":\"self\",\"color\":\"#e1d447\",\"pose\":{\"t\":{\"p\":[9,9,9],\"q\":[0,0,0,1]}}},")
        TEXT("{\"id\":\"B\",\"color\":\"#47e19a\",\"pose\":{\"t\":{\"p\":[1,1,1],\"q\":[0,0,0,1]}}},")
        TEXT("{\"id\":\"C\",\"color\":\"#e14760\",\"pose\":{\"t\":{\"p\":[0,1.6,0],\"q\":[0,0,0,1]},\"hint\":\"vr-head\"}},")
        TEXT("{\"id\":\"D\",\"color\":\"#e19a47\",\"pose\":null}]}"));
    TestEqual(TEXT("B and C, not ourselves or D"), Access.Num(), 2);
    const FLoomaClientPose* B = Access.Find(TEXT("B"));
    const FLoomaClientPose* C = Access.Find(TEXT("C"));
    if (!TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C))
    {
        return false;
    }
    TestEqual(TEXT("B keeps its fresher pose"), B->Head.GetLocation().Y, 300.0, 0.01);
    TestEqual(TEXT("B takes the roster's colour"), B->ColorHex, FString(TEXT("#47e19a")));
    TestEqual(TEXT("C is hydrated from the roster"), C->Head.GetLocation().Z, 160.0, 0.01);

    // B leaves the roster, then the socket drops.
    Access.Receive(TEXT("{\"type\":\"clients\",\"you\":\"self\",\"clients\":[")
        TEXT("{\"id\":\"C\",\"color\":\"#e14760\",\"pose\":{\"t\":{\"p\":[0,1.6,0],\"q\":[0,0,0,1]}}}]}"));
    TestNull(TEXT("somebody who left is nowhere"), Access.Find(TEXT("B")));
    TestNotNull(TEXT("C stays"), Access.Find(TEXT("C")));
    Access.Disconnect();
    TestEqual(TEXT("a dropped socket forgets everyone"), Access.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaClientPoseParseTest,
    "Looma.Presence.ClientPose.Parse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaClientPoseParseTest::RunTest(const FString& Parameters)
{
    FLoomaClientPose Pose;
    TestFalse(TEXT("no t"), LoomaParseClientPose(ParseObject(TEXT("{\"hint\":\"vr-head\"}")), Pose));
    TestFalse(TEXT("a coordinate short"),
        LoomaParseClientPose(ParseObject(TEXT("{\"t\":{\"p\":[0,1],\"q\":[0,0,0,1]}}")), Pose));
    TestFalse(TEXT("a string for a number"),
        LoomaParseClientPose(ParseObject(TEXT("{\"t\":{\"p\":[0,\"1\",0],\"q\":[0,0,0,1]}}")), Pose));
    TestTrue(TEXT("a bare head"),
        LoomaParseClientPose(ParseObject(TEXT("{\"t\":{\"p\":[0,1,0],\"q\":[0,0,0,1]}}")), Pose));
    TestFalse(TEXT("no hands"), Pose.bHasLeftHand || Pose.bHasRightHand);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaClientPoseFigureTest,
    "Looma.Presence.ClientPose.Figure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaClientPoseFigureTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Values);
    ALoomaRemotePerson* Person = World->SpawnActor<ALoomaRemotePerson>();
    if (!TestNotNull(TEXT("a figure spawns"), Person))
    {
        World->DestroyWorld(false);
        return false;
    }

    FLoomaClientPose Pose;
    Pose.Head = FTransform(FRotator(0.0, 90.0, 0.0), FVector(100.0, 200.0, 160.0));
    Pose.bHasRightHand = true;
    Pose.RightHand = FTransform(FVector(120.0, 230.0, 120.0));
    Person->SetLook(FLinearColor::Red, TEXT("Nyx"));
    Person->Follow(Pose, 0.25f, true, FVector(500.0, 200.0, 160.0));

    TestTrue(TEXT("the head is where the pose says, at once"),
        Person->Head->GetComponentLocation().Equals(Pose.Head.GetLocation(), 0.01));
    TestTrue(TEXT("the visor is on the side the head looks out of"),
        (Person->Visor->GetComponentLocation() - Person->Head->GetComponentLocation()).GetSafeNormal()
            .Equals(Pose.Head.GetRotation().GetForwardVector(), 0.2));
    TestTrue(TEXT("the right hand is shown"), Person->RightHand->IsVisible());
    TestFalse(TEXT("an untracked left hand is not"), Person->LeftHand->IsVisible());
    TestEqual(TEXT("the name"), Person->NameTag->Text.ToString(), FString(TEXT("Nyx")));
    TestTrue(TEXT("the name is above the head"),
        Person->NameTag->GetComponentLocation().Z > Person->Head->GetComponentLocation().Z);
    TestEqual(TEXT("nothing traces against it"), Person->Head->GetCollisionEnabled(), ECollisionEnabled::NoCollision);

    // Later poses are eased towards, not jumped to.
    FLoomaClientPose Moved = Pose;
    Moved.Head.SetLocation(FVector(100.0, 300.0, 160.0));
    Person->Follow(Moved, 0.25f, false, FVector(500.0, 200.0, 160.0));
    TestEqual(TEXT("a quarter of the way"), Person->Head->GetComponentLocation().Y, 225.0, 0.01);

    World->DestroyWorld(false);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
