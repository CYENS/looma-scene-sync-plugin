#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

/**
 * No Initialize: that would restore a real session and open a real socket. The
 * fixture only seeds tracked actors in a transient world; edits then enter through
 * the same JSON dispatcher as WebSocket frames. Assertions read the primitives and
 * public draw list after Tick, so a forgotten invalidation cannot pass merely because
 * the underlying hierarchy or component array is correct.
 */
struct FLoomaBorderRefreshTestFixture
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{
        NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};
    UWorld* World = nullptr;

    FLoomaBorderRefreshTestFixture()
    {
        Sync->ClientId = TEXT("self");
        const UWorld::InitializationValues Values = UWorld::InitializationValues()
            .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
        // These tests inspect stencil ownership, not the host project's optional
        // post-process setup. Its two diagnostics are deliberately outside this test.
        Sync->bWarnedCustomDepthOff = true;
        Sync->bWarnedNoBorderCollection = true;
    }

    ~FLoomaBorderRefreshTestFixture()
    {
        Sync->Tracked.Reset();
        World->DestroyWorld(false);
    }

    ALoomaSyncedActor* Seed(const TCHAR* Id)
    {
        ALoomaSyncedActor* Actor = World->SpawnActor<ALoomaSyncedActor>();
        Actor->Id = Id;
        FLoomaTrackedActor Entry;
        Entry.Actor = Actor;
        Sync->Tracked.Add(Id, Entry);
        return Actor;
    }

    void Receive(const TCHAR* Json) { Sync->OnRawMessage(Json); }
    void Tick() { Sync->Tick(1.0f / 60.0f); }

    void PollLocalAttachment()
    {
        // The public tick normally gates this diff on an open socket. Exercise the
        // actual diff without fabricating connectivity or sending to a live room.
        Sync->TickOutbound(1.0f / 60.0f);
        Tick();
    }

    void ClaimParent()
    {
        Receive(TEXT(R"({"type":"clients","you":"self","clients":[{"id":"self"},{"id":"remote","color":"#ff0000","selection":["parent"]}]})"));
        Tick();
    }

    void ExpectStencil(FAutomationTestBase& Test, const TCHAR* Id, int32 Expected)
    {
        ALoomaSyncedActor* Actor = Sync->FindSyncedActor(Id);
        UStaticMeshComponent* Mesh = Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
        if (Test.TestNotNull(FString::Printf(TEXT("%s has a primitive"), Id), Mesh))
        {
            Test.TestEqual(FString::Printf(TEXT("%s stencil"), Id), Mesh->CustomDepthStencilValue, Expected);
            Test.TestEqual(FString::Printf(TEXT("%s custom depth"), Id), Mesh->bRenderCustomDepth != 0, Expected != 0);
        }
    }

    bool HasChildHint(const TCHAR* Id) const
    {
        for (const FLoomaBorderGroup& Group : Sync->GetRemoteBorderGroups())
        {
            if (Group.ClientId == TEXT("remote") && Group.ChildNodeIds.Contains(Id))
            {
                return true;
            }
        }
        return false;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaBorderRemoteReparentTest,
    "Looma.Presence.SceneRefresh.RemoteReparent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaBorderRemoteReparentTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    Fixture.Seed(TEXT("parent"));
    Fixture.Seed(TEXT("child"));
    Fixture.Receive(TEXT(R"({"type":"spawn","nodes":[{"id":"parent"},{"id":"child","parent":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    Fixture.ClaimParent();
    Fixture.ExpectStencil(*this, TEXT("child"), 129);

    Fixture.Receive(TEXT(R"({"type":"reparent","nodes":[{"id":"child","parent":null}]})"));
    Fixture.Tick();
    Fixture.ExpectStencil(*this, TEXT("child"), 0);
    TestFalse(TEXT("Detached child leaves the published hint list"), Fixture.HasChildHint(TEXT("child")));

    Fixture.Receive(TEXT(R"({"type":"reparent","nodes":[{"id":"child","parent":"parent"}]})"));
    Fixture.Tick();
    Fixture.ExpectStencil(*this, TEXT("child"), 129);
    TestTrue(TEXT("Reattached child returns to the published hint list"), Fixture.HasChildHint(TEXT("child")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaBorderComponentPatchTest,
    "Looma.Presence.SceneRefresh.ComponentPatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaBorderComponentPatchTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    Fixture.Seed(TEXT("parent"));
    Fixture.ClaimParent();

    Fixture.Receive(TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    Fixture.Tick();
    Fixture.ExpectStencil(*this, TEXT("parent"), 1);

    UStaticMeshComponent* First = Fixture.Sync->FindSyncedActor(TEXT("parent"))->FindComponentByClass<UStaticMeshComponent>();
    Fixture.Receive(TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[]}]})"));
    Fixture.Tick();
    TestNull(TEXT("Clearing components removes the outlined mesh"),
        Fixture.Sync->FindSyncedActor(TEXT("parent"))->FindComponentByClass<UStaticMeshComponent>());

    Fixture.Receive(TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[{"type":"mesh","shape":"sphere"}]}]})"));
    Fixture.Tick();
    TestTrue(TEXT("Re-adding creates a different primitive"),
        First != Fixture.Sync->FindSyncedActor(TEXT("parent"))->FindComponentByClass<UStaticMeshComponent>());
    Fixture.ExpectStencil(*this, TEXT("parent"), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaBorderExistingUpsertTest,
    "Looma.Presence.SceneRefresh.ExistingUpsert",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaBorderExistingUpsertTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    Fixture.Seed(TEXT("parent"));
    Fixture.Seed(TEXT("child"));
    Fixture.ClaimParent();

    // An echoed/normalised spawn can upsert a known node. There is no fresh actor to
    // dirty the scene for us; both the new mesh and its parent must reach the border.
    Fixture.Receive(TEXT(R"({"type":"spawn","nodes":[{"id":"parent","components":[{"type":"mesh","shape":"box"}]},{"id":"child","parent":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    Fixture.Tick();
    Fixture.ExpectStencil(*this, TEXT("parent"), 1);
    Fixture.ExpectStencil(*this, TEXT("child"), 129);
    TestTrue(TEXT("Upserted child enters the published hint list"), Fixture.HasChildHint(TEXT("child")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaBorderLocalReparentTest,
    "Looma.Presence.SceneRefresh.LocalReparent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaBorderLocalReparentTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    ALoomaSyncedActor* Parent = Fixture.Seed(TEXT("parent"));
    ALoomaSyncedActor* Child = Fixture.Seed(TEXT("child"));
    Fixture.Receive(TEXT(R"({"type":"spawn","nodes":[{"id":"parent"},{"id":"child","parent":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    Fixture.ClaimParent();
    Fixture.ExpectStencil(*this, TEXT("child"), 129);

    Child->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    Fixture.PollLocalAttachment();
    TestTrue(TEXT("Outbound diff records local detach"), Child->ParentId.IsEmpty());
    Fixture.ExpectStencil(*this, TEXT("child"), 0);
    TestFalse(TEXT("Locally detached child leaves the published hint list"), Fixture.HasChildHint(TEXT("child")));

    Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
    Fixture.PollLocalAttachment();
    Fixture.ExpectStencil(*this, TEXT("child"), 129);
    TestTrue(TEXT("Locally attached child returns to the published hint list"), Fixture.HasChildHint(TEXT("child")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
