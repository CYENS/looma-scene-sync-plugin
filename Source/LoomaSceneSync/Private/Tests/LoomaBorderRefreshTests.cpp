#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Tests/LoomaPresenceTestObserver.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

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
    void Disconnect() { Sync->ClearPresence(); }

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaPresenceColorOnlyTest,
    "Looma.Presence.Coherence.ColorOnlySelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaPresenceColorOnlyTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    Fixture.Seed(TEXT("parent"));
    TStrongObjectPtr<ULoomaPresenceTestObserver> Observer{NewObject<ULoomaPresenceTestObserver>()};
    int32 Notifications = 0;
    Observer->OnClients = [&](const TArray<FLoomaClient>& Clients) { ++Notifications; };
    Fixture.Sync->OnClientsChanged.AddDynamic(Observer.Get(), &ULoomaPresenceTestObserver::ObserveClients);
    Fixture.ClaimParent();
    TestEqual(TEXT("Initial roster notifies once"), Notifications, 1);

    Fixture.Receive(TEXT(R"({"type":"selection","clientId":"remote","color":"#00ff00","ids":["parent"]})"));
    Fixture.Tick();
    TestEqual(TEXT("Same selection with a new colour notifies the room"), Notifications, 2);
    const TArray<FLoomaBorderGroup> Groups = Fixture.Sync->GetRemoteBorderGroups();
    if (TestEqual(TEXT("The existing border remains allocated"), Groups.Num(), 1))
    {
        TestEqual(TEXT("The draw list receives the new colour"), Groups[0].Color, FLinearColor::Green);
    }
    const TArray<FString> Claimants = Fixture.Sync->GetNodeClaimants(TEXT("parent"));
    TestTrue(TEXT("Colour-only update keeps the claimant in place"), Claimants == TArray<FString>{TEXT("remote")});

    Fixture.Receive(TEXT(R"({"type":"selection","clientId":"remote","color":"#0f0","ids":["parent"]})"));
    Fixture.Tick();
    TestEqual(TEXT("Equivalent colour and selection do not notify again"), Notifications, 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaPresenceEmptyProvisionalTest,
    "Looma.Presence.Coherence.EmptyProvisionalClient",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaPresenceEmptyProvisionalTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    TStrongObjectPtr<ULoomaPresenceTestObserver> Observer{NewObject<ULoomaPresenceTestObserver>()};
    int32 Notifications = 0;
    TArray<FLoomaClient> Observed;
    Observer->OnClients = [&](const TArray<FLoomaClient>& Clients) { ++Notifications; Observed = Clients; };
    Fixture.Sync->OnClientsChanged.AddDynamic(Observer.Get(), &ULoomaPresenceTestObserver::ObserveClients);

    Fixture.Receive(TEXT(R"({"type":"selection","clientId":"newcomer","color":"#123456","ids":[]})"));
    Fixture.Tick();
    TestEqual(TEXT("An empty first selection still introduces its sender"), Notifications, 1);
    if (TestEqual(TEXT("Callback receives the provisional client"), Observed.Num(), 1))
    {
        TestEqual(TEXT("Inline server colour is retained"), Observed[0].ColorHex, FString(TEXT("#123456")));
    }
    Fixture.Receive(TEXT(R"({"type":"selection","clientId":"newcomer","color":"#123456","ids":[]})"));
    Fixture.Tick();
    TestEqual(TEXT("Identical provisional update is quiet"), Notifications, 1);
    TestEqual(TEXT("An empty selection creates no borders"), Fixture.Sync->GetRemoteBorderGroups().Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaPresenceRefreshCallbackTest,
    "Looma.Presence.Coherence.RefreshCallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaPresenceRefreshCallbackTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    ALoomaSyncedActor* Parent = Fixture.Seed(TEXT("parent"));
    Fixture.Receive(TEXT(R"({"type":"spawn","nodes":[{"id":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    TStrongObjectPtr<ULoomaPresenceTestObserver> Observer{NewObject<ULoomaPresenceTestObserver>()};
    int32 RoomNotifications = 0;
    int32 BorderNotifications = 0;
    int32 ObservedStencil = -1;
    TArray<FLoomaBorderGroup> ObservedGroups;
    Observer->OnClients = [&](const TArray<FLoomaClient>& Clients) { ++RoomNotifications; };
    Observer->OnBorders = [&]()
    {
        ++BorderNotifications;
        ObservedGroups = Fixture.Sync->GetRemoteBorderGroups();
        UStaticMeshComponent* Mesh = Parent->FindComponentByClass<UStaticMeshComponent>();
        ObservedStencil = Mesh ? Mesh->CustomDepthStencilValue : -1;
    };
    Fixture.Sync->OnClientsChanged.AddDynamic(Observer.Get(), &ULoomaPresenceTestObserver::ObserveClients);
    // Reflection keeps the regression executable against the old module too, where
    // no post-refresh event exists. Once present, this binds a real Blueprint delegate.
    FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(
        Fixture.Sync->GetClass(), TEXT("OnRemoteBordersRefreshed"));
    if (!TestNotNull(TEXT("Blueprint exposes a post-refresh border event"), Property))
    {
        return false;
    }
    FScriptDelegate Callback;
    Callback.BindUFunction(Observer.Get(), GET_FUNCTION_NAME_CHECKED(ULoomaPresenceTestObserver, ObserveBorders));
    Property->AddDelegate(Callback, Fixture.Sync.Get());

    Fixture.Receive(TEXT(R"({"type":"clients","you":"self","clients":[{"id":"self"},{"id":"remote","color":"#ff0000","selection":["parent"]}]})"));
    TestEqual(TEXT("Roster callbacks stay immediate"), RoomNotifications, 1);
    TestEqual(TEXT("Rendering stays coalesced until the tick"), BorderNotifications, 0);
    Fixture.Tick();
    TestEqual(TEXT("First dirty tick publishes once"), BorderNotifications, 1);
    TestEqual(TEXT("Getter is already current inside callback"), ObservedGroups.Num(), 1);
    TestEqual(TEXT("Primitive is already marked inside callback"), ObservedStencil, 1);
    Fixture.Tick();
    TestEqual(TEXT("An unchanged frame sends no refresh callback"), BorderNotifications, 1);

    Fixture.Receive(TEXT(R"({"type":"selection","clientId":"remote","ids":[]})"));
    Fixture.Receive(TEXT(R"({"type":"selection","clientId":"remote","ids":["parent"]})"));
    Fixture.Tick();
    TestEqual(TEXT("Two same-frame changes share one border callback"), BorderNotifications, 2);
    TestEqual(TEXT("Callback observes the final coalesced selection"), ObservedGroups.Num(), 1);

    Fixture.Sync->SetLocalSelection({Parent});
    Fixture.Tick();
    TestEqual(TEXT("Local selection alone refreshes borders"), BorderNotifications, 3);
    TestEqual(TEXT("Local priority is visible in callback getter"), ObservedGroups.Num(), 0);
    TestEqual(TEXT("Local priority has already cleared remote stencil"), ObservedStencil, 0);
    TestEqual(TEXT("Local selection causes no remote roster event"), RoomNotifications, 3);

    Fixture.Sync->ClearSelection();
    Fixture.Tick();
    Fixture.Receive(TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[]}]})"));
    Fixture.Receive(TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[{"type":"mesh","shape":"sphere"}]}]})"));
    Fixture.Tick();
    TestEqual(TEXT("Component replacement also refreshes consumers"), BorderNotifications, 5);
    TestEqual(TEXT("Callback sees the new component already marked"), ObservedStencil, 1);

    Fixture.Disconnect();
    Fixture.Tick();
    TestEqual(TEXT("Disconnect produces the final empty callback"), BorderNotifications, 6);
    TestEqual(TEXT("Disconnected getter is empty inside callback"), ObservedGroups.Num(), 0);
    TestEqual(TEXT("Disconnect stencil cleanup precedes callback"), ObservedStencil, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaPresenceSceneIdentityTest,
    "Looma.Presence.Coherence.SceneIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaPresenceSceneIdentityTest::RunTest(const FString& Parameters)
{
    FLoomaBorderRefreshTestFixture Fixture;
    ALoomaSyncedActor* Parent = Fixture.Seed(TEXT("parent"));
    Fixture.ClaimParent();
    Fixture.Receive(TEXT(R"({"type":"scene","sceneId":"scene-a","performance":{"id":"show"},"nodes":[{"id":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    Fixture.Tick();
    Fixture.ExpectStencil(*this, TEXT("parent"), 1);
    TestEqual(TEXT("Initial scene preserves presence that arrived first"), Fixture.Sync->GetClients().Num(), 1);

    Fixture.Receive(TEXT(R"({"type":"scene","sceneId":"scene-a","performance":{"id":"show","name":"Renamed show"},"nodes":[{"id":"parent","components":[{"type":"mesh","shape":"sphere"}]}]})"));
    Fixture.Tick();
    Fixture.ExpectStencil(*this, TEXT("parent"), 1);
    TestEqual(TEXT("Same scene resync preserves its live claims"), Fixture.Sync->GetNodeClaimants(TEXT("parent")).Num(), 1);

    Fixture.Sync->SetLocalSelection({Parent});
    Fixture.Tick();
    Fixture.Receive(TEXT(R"({"type":"scene","sceneId":"scene-a","performance":{"id":"show"},"nodes":[{"id":"parent","components":[{"type":"mesh","shape":"sphere"}]}]})"));
    Fixture.Tick();
    TestEqual(TEXT("Same-scene resync preserves local selection too"), Fixture.Sync->GetLocalSelectionIds().Num(), 1);
    TestEqual(TEXT("Local priority does not retract the remote claim"), Fixture.Sync->GetNodeClaimants(TEXT("parent")).Num(), 1);
    Fixture.Receive(TEXT(R"({"type":"scene","sceneId":"scene-b","performance":{"id":"show"},"nodes":[{"id":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    TestEqual(TEXT("A scene change clears the room before new roster hydration"), Fixture.Sync->GetClients().Num(), 0);
    TestEqual(TEXT("A reused node id inherits no remote claim"), Fixture.Sync->GetNodeClaimants(TEXT("parent")).Num(), 0);
    TestEqual(TEXT("A reused actor inherits no local selection"), Fixture.Sync->GetLocalSelectionIds().Num(), 0);
    Fixture.Tick();
    Fixture.ExpectStencil(*this, TEXT("parent"), 0);

    Fixture.Receive(TEXT(R"({"type":"clients","you":"self","clients":[{"id":"self"},{"id":"remote","color":"#00ff00","selection":[]}]})"));
    Fixture.Tick();
    TestEqual(TEXT("New-scene roster cannot revive an old-scene selection"), Fixture.Sync->GetNodeClaimants(TEXT("parent")).Num(), 0);
    Fixture.ClaimParent();
    Fixture.Receive(TEXT(R"({"type":"selection","clientId":"remote","ids":["parent"]})"));
    Fixture.Tick();
    Fixture.Receive(TEXT(R"({"type":"scene","sceneId":"scene-b","performance":{"id":"other-show"},"nodes":[{"id":"parent","components":[{"type":"mesh","shape":"box"}]}]})"));
    Fixture.Tick();
    TestEqual(TEXT("Performance identity also scopes presence"), Fixture.Sync->GetClients().Num(), 0);
    Fixture.ExpectStencil(*this, TEXT("parent"), 0);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
