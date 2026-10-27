#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "LoomaSceneComponents.h"
#include "LoomaSyncedActor.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

/**
 * The `hidden` component (looma-xr-asset-demo docs/scene-format.md, "The `hidden`
 * component"): parsed as the node's own switch, and drawn as the format says, which
 * is a whole subtree switched off. A child is hidden under a hidden parent whatever
 * its own switch says, comes back when it leaves, and nothing hidden collides or
 * lights anything. Frames enter through the socket's dispatcher; nothing is opened
 * or fetched.
 */

namespace
{
    // Prefixed: the plugin's test files share one unity translation unit.
    const TCHAR* const HiddenTestBox = TEXT(R"({"type":"mesh","shape":"box"})");
    const TCHAR* const HiddenTestOff = TEXT(R"({"type":"hidden"})");

    FLoomaNodeComponents HiddenTestParse(const TCHAR* ArrayJson)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ArrayJson), Values);
        return LoomaParseComponents(&Values);
    }
}

/** Nodes in a transient world. No Initialize, so no session and no socket. */
struct FLoomaHiddenTestFixture
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};
    UWorld* World = nullptr;

    FLoomaHiddenTestFixture()
    {
        const UWorld::InitializationValues Values = UWorld::InitializationValues()
            .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
    }

    ~FLoomaHiddenTestFixture()
    {
        Sync->Tracked.Reset();
        World->DestroyWorld(false);
    }

    /** The subsystem spawns into the game instance's world, which this one has none of. */
    void Seed(const TCHAR* Id)
    {
        ALoomaSyncedActor* Actor = World->SpawnActor<ALoomaSyncedActor>();
        Actor->Id = Id;
        FLoomaTrackedActor Entry;
        Entry.Actor = Actor;
        Sync->Tracked.Add(Id, Entry);
    }

    void Receive(const FString& Json) { Sync->OnRawMessage(Json); }

    /** A node as the hub spawns it, with these components and this parent ("" for a root). */
    void Spawn(const TCHAR* Id, const TCHAR* Parent, const FString& Components)
    {
        const FString ParentJson = *Parent ? FString::Printf(TEXT(R"("%s")"), Parent) : TEXT("null");
        Receive(FString::Printf(TEXT(R"({"type":"spawn","nodes":[{"id":"%s","parent":%s,"components":[%s]}]})"),
            Id, *ParentJson, *Components));
    }

    void Patch(const TCHAR* Id, const FString& Components)
    {
        Receive(FString::Printf(TEXT(R"({"type":"patch","nodes":[{"id":"%s","components":[%s]}]})"),
            Id, *Components));
    }

    void Reparent(const TCHAR* Id, const TCHAR* Parent)
    {
        const FString ParentJson = *Parent ? FString::Printf(TEXT(R"("%s")"), Parent) : TEXT("null");
        Receive(FString::Printf(TEXT(R"({"type":"reparent","nodes":[{"id":"%s","parent":%s}]})"), Id, *ParentJson));
    }

    ALoomaSyncedActor* Node(const TCHAR* Id) const { return Sync->FindSyncedActor(Id); }

    /** Hidden or shown in every way that matters: drawn, collided with, and the flag that says so. */
    void Expect(FAutomationTestBase& Test, const TCHAR* Id, bool bHidden)
    {
        ALoomaSyncedActor* Actor = Node(Id);
        if (!Test.TestNotNull(FString::Printf(TEXT("%s exists"), Id), Actor))
        {
            return;
        }
        const TCHAR* Want = bHidden ? TEXT("hidden") : TEXT("shown");
        Test.TestEqual(FString::Printf(TEXT("%s is %s"), Id, Want), Actor->bHiddenInScene, bHidden);
        Test.TestEqual(FString::Printf(TEXT("%s's actor is %s in game"), Id, Want), Actor->IsHidden(), bHidden);
        Test.TestEqual(FString::Printf(TEXT("%s collides only while shown"), Id),
            Actor->GetActorEnableCollision(), !bHidden);
        if (UStaticMeshComponent* Mesh = Actor->MeshComponent.Get())
        {
            Test.TestEqual(FString::Printf(TEXT("%s's mesh renders only while shown"), Id), Mesh->ShouldRender(), !bHidden);
        }
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaHiddenParseTest,
    "Looma.Hidden.Parse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaHiddenParseTest::RunTest(const FString& Parameters)
{
    const FLoomaNodeComponents Off = HiddenTestParse(TEXT(R"([{"type":"mesh","shape":"box"},{"type":"hidden"}])"));
    TestTrue(TEXT("`hidden` switches the node off"), Off.bHidden);
    TestTrue(TEXT("and is known, not logged as an unknown type"), Off.UnknownTypes.IsEmpty());
    TestTrue(TEXT("the mesh beside it is kept"), Off.bHasMesh);

    const FLoomaNodeComponents On = HiddenTestParse(TEXT(R"([{"type":"mesh","shape":"box"}])"));
    TestFalse(TEXT("no `hidden`, shown"), On.bHidden);
    TestTrue(TEXT("hiding is a change of components"), Off != On);

    const FLoomaNodeComponents Alone = HiddenTestParse(TEXT(R"([{"type":"hidden"}])"));
    TestTrue(TEXT("a node with only `hidden` is still an empty object"), Alone.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaHiddenSubtreeTest,
    "Looma.Hidden.Subtree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaHiddenSubtreeTest::RunTest(const FString& Parameters)
{
    FLoomaHiddenTestFixture F;
    F.Seed(TEXT("table"));
    F.Seed(TEXT("tray"));
    F.Seed(TEXT("cup"));
    F.Spawn(TEXT("table"), TEXT(""), FString::Printf(TEXT("%s,%s"), HiddenTestBox, HiddenTestOff));
    F.Spawn(TEXT("tray"), TEXT("table"), HiddenTestBox);
    F.Spawn(TEXT("cup"), TEXT("tray"), HiddenTestBox);

    // A hidden table takes everything on it, two levels down.
    F.Expect(*this, TEXT("table"), true);
    F.Expect(*this, TEXT("tray"), true);
    F.Expect(*this, TEXT("cup"), true);
    TestFalse(TEXT("the tray's own switch is still on"), F.Node(TEXT("tray"))->GetComponents().bHidden);

    // Shown again, they all come back.
    F.Patch(TEXT("table"), HiddenTestBox);
    F.Expect(*this, TEXT("table"), false);
    F.Expect(*this, TEXT("tray"), false);
    F.Expect(*this, TEXT("cup"), false);

    // Hiding the middle one leaves the table alone.
    F.Patch(TEXT("tray"), FString::Printf(TEXT("%s,%s"), HiddenTestBox, HiddenTestOff));
    F.Expect(*this, TEXT("table"), false);
    F.Expect(*this, TEXT("tray"), true);
    F.Expect(*this, TEXT("cup"), true);

    // A table hidden over a hidden tray, then shown: the tray stays hidden, by its own switch.
    F.Patch(TEXT("table"), FString::Printf(TEXT("%s,%s"), HiddenTestBox, HiddenTestOff));
    F.Patch(TEXT("table"), HiddenTestBox);
    F.Expect(*this, TEXT("tray"), true);
    F.Expect(*this, TEXT("cup"), true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaHiddenReparentTest,
    "Looma.Hidden.Reparent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaHiddenReparentTest::RunTest(const FString& Parameters)
{
    FLoomaHiddenTestFixture F;
    F.Seed(TEXT("curtain"));
    F.Seed(TEXT("prop"));
    F.Spawn(TEXT("curtain"), TEXT(""), FString::Printf(TEXT("%s,%s"), HiddenTestBox, HiddenTestOff));
    F.Spawn(TEXT("prop"), TEXT(""), HiddenTestBox);
    F.Expect(*this, TEXT("prop"), false);

    F.Reparent(TEXT("prop"), TEXT("curtain"));
    F.Expect(*this, TEXT("prop"), true);

    F.Reparent(TEXT("prop"), TEXT(""));
    F.Expect(*this, TEXT("prop"), false);

    // A parent that arrives after its child, hidden, still hides it once it attaches
    // (ResolvePendingParents). Until then the child is a root, and shown.
    F.Seed(TEXT("early"));
    F.Spawn(TEXT("early"), TEXT("late"), HiddenTestBox);
    F.Expect(*this, TEXT("early"), false);
    F.Seed(TEXT("late"));
    F.Spawn(TEXT("late"), TEXT(""), FString::Printf(TEXT("%s,%s"), HiddenTestBox, HiddenTestOff));
    TestTrue(TEXT("the early child found its parent"),
        F.Node(TEXT("early"))->GetAttachParentActor() == F.Node(TEXT("late")));
    F.Expect(*this, TEXT("early"), true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaHiddenLightTest,
    "Looma.Hidden.Light",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaHiddenLightTest::RunTest(const FString& Parameters)
{
    FLoomaHiddenTestFixture F;
    F.Seed(TEXT("lamp"));
    const TCHAR* const LampLight = TEXT(R"({"type":"light","lightType":"point","intensity":2})");
    F.Spawn(TEXT("lamp"), TEXT(""), FString::Printf(TEXT("%s,%s"), LampLight, HiddenTestOff));

    ALoomaSyncedActor* LampActor = F.Node(TEXT("lamp"));
    if (!TestNotNull(TEXT("the lamp exists"), LampActor) || !TestNotNull(TEXT("it has its light"), LampActor->LightComponent.Get()))
    {
        return false;
    }
    // A light is kept, not destroyed, and is simply not added to the scene.
    TestFalse(TEXT("a hidden lamp lights nothing"), LampActor->LightComponent->ShouldRender());

    F.Patch(TEXT("lamp"), LampLight);
    TestTrue(TEXT("shown, it lights again"), LampActor->LightComponent->ShouldRender());
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
