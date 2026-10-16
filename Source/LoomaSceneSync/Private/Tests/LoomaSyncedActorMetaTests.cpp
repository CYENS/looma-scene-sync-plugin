#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSyncedActor.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/LoomaGenerationTestObserver.h"
#include "UObject/StrongObjectPtr.h"

/**
 * A synced actor's generation meta (ALoomaSyncedActor::MetaJson): read off the job
 * that made the node's asset, whichever of the two arrives first, from the newest
 * such job, from a sound only on a node with no model, and dropped when the node
 * changes to another asset. Frames enter through the socket's dispatcher and jobs
 * through the hydrate's, and nothing leaves the machine.
 */

namespace
{
    const TCHAR* const LevelMeta = TEXT(R"({"unreal":{"environment":"LVL_Demo"}})");
    const TCHAR* const OtherMeta = TEXT(R"({"unreal":{"environment":"LVL_Other"}})");

    /** A finished job as `GET /generate` lists it (snake_case). Meta is JSON text, "null" for none. */
    FString ListedJob(const TCHAR* JobId, const TCHAR* AssetId, const TCHAR* CreatedAt, const TCHAR* Meta)
    {
        return FString::Printf(
            TEXT(R"({"job_id":"%s","prompt":"a chair","state":"done","progress":1,"asset_id":"%s","created_at":"%s","meta":%s})"),
            JobId, AssetId, CreatedAt, Meta);
    }

    /** The same job finishing over the socket (camelCase). */
    FString DoneEvent(const TCHAR* JobId, const TCHAR* AssetId, const TCHAR* CreatedAt, const TCHAR* Meta)
    {
        return FString::Printf(
            TEXT(R"({"type":"generation","job":{"jobId":"%s","prompt":"a chair","state":"done","progress":1,"assetId":"%s","createdAt":"%s","meta":%s}})"),
            JobId, AssetId, CreatedAt, Meta);
    }

    FString Model(const TCHAR* AssetId)
    {
        return FString::Printf(TEXT(R"({"type":"model","assetId":"%s"})"), AssetId);
    }

    FString Sound(const TCHAR* AssetId)
    {
        return FString::Printf(TEXT(R"({"type":"audio","assetId":"%s"})"), AssetId);
    }
}

/** Nodes in a transient world, every download slot held so no GLB or WAV is fetched. */
struct FLoomaSyncedActorMetaTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};
    TStrongObjectPtr<ULoomaGenerationTestObserver> Observer{NewObject<ULoomaGenerationTestObserver>()};
    UWorld* World = nullptr;

    FLoomaSyncedActorMetaTestAccess()
    {
        const UWorld::InitializationValues Values = UWorld::InitializationValues()
            .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
        for (int32 Slot = 0; Slot < 4; ++Slot)
        {
            Sync->GlbInFlight.Add(FHttpModule::Get().CreateRequest());
        }
        Sync->OnSyncedActorMetaChanged.AddDynamic(Observer.Get(), &ULoomaGenerationTestObserver::ObserveActorMeta);
    }

    ~FLoomaSyncedActorMetaTestAccess()
    {
        Sync->GlbQueue.Reset();
        Sync->GlbInFlight.Reset();
        Sync->Tracked.Reset();
        World->DestroyWorld(false);
    }

    /** A node with these components, arriving as the hub sends one: seeded, then patched. */
    ALoomaSyncedActor* Node(const TCHAR* Id, const FString& Components)
    {
        if (!Sync->FindSyncedActor(Id))
        {
            ALoomaSyncedActor* Actor = World->SpawnActor<ALoomaSyncedActor>();
            Actor->Id = Id;
            FLoomaTrackedActor Entry;
            Entry.Actor = Actor;
            Sync->Tracked.Add(Id, Entry);
        }
        Sync->OnRawMessage(FString::Printf(
            TEXT(R"({"type":"patch","nodes":[{"id":"%s","components":[%s]}]})"), Id, *Components));
        return Sync->FindSyncedActor(Id);
    }

    /** One job of the connect-time `GET /generate`, applied as its response handler does. */
    void Listed(const FString& Job)
    {
        TSharedPtr<FJsonObject> Object;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Job), Object);
        Sync->ApplyJob(LoomaParseGenerationJob(Object));
    }

    void Receive(const FString& Json) { Sync->OnRawMessage(Json); }

    /** Every `On Synced Actor Meta Changed` so far, in order. */
    FString Changes() const { return FString::Join(Observer->ActorMetas, TEXT(" | ")); }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaSyncedActorMetaOrderTest,
    "Looma.Generation.Meta.Actor.Order",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaSyncedActorMetaOrderTest::RunTest(const FString& Parameters)
{
    FLoomaSyncedActorMetaTestAccess Access;

    // The scene first, as on every connect: the job list has not answered yet.
    ALoomaSyncedActor* Seat = Access.Node(TEXT("chair"), Model(TEXT("gen-a")));
    TestFalse(TEXT("not known before its job is"), Seat->bHasMeta);
    TestTrue(TEXT("and empty"), Seat->MetaJson.IsEmpty());
    TestEqual(TEXT("nothing said yet"), Access.Changes(), FString());

    // No jobId on the node: the asset is enough.
    Access.Listed(ListedJob(TEXT("j1"), TEXT("gen-a"), TEXT("2026-10-16T09:00:00"), LevelMeta));
    TestTrue(TEXT("the job brings it"), Seat->bHasMeta);
    TestEqual(TEXT("as the job holds it"), Seat->MetaJson, FString(LevelMeta));
    TestEqual(TEXT("said once"), Access.Changes(), FString(TEXT("chair=")) + LevelMeta);

    FString Environment;
    TestTrue(TEXT("readable by path"), Seat->GetGenerationMetaString(TEXT("unreal.environment"), Environment));
    TestEqual(TEXT("the value"), Environment, FString(TEXT("LVL_Demo")));
    TestFalse(TEXT("a path not in it"), Seat->GetGenerationMetaString(TEXT("unreal.level"), Environment));
    TestTrue(TEXT("leaves the value empty"), Environment.IsEmpty());

    // A reconnect lists the same job again: nothing changed, so nothing is said.
    Access.Listed(ListedJob(TEXT("j1"), TEXT("gen-a"), TEXT("2026-10-16T09:00:00"), LevelMeta));
    TestEqual(TEXT("not said twice"), Access.Observer->ActorMetas.Num(), 1);

    // The job first: a node showing the same asset arrives already knowing it.
    ALoomaSyncedActor* Copy = Access.Node(TEXT("copy"), Model(TEXT("gen-a")));
    TestEqual(TEXT("a second node of the asset"), Copy->MetaJson, FString(LevelMeta));
    const FString BothSaid = FString::Printf(TEXT("chair=%s | copy=%s"), LevelMeta, LevelMeta);
    TestEqual(TEXT("is said on arrival, for that node"), Access.Changes(), BothSaid);

    // A job that never had a meta gives its node none, and says nothing.
    ALoomaSyncedActor* Plain = Access.Node(TEXT("plain"), Model(TEXT("gen-b")));
    Access.Listed(ListedJob(TEXT("j2"), TEXT("gen-b"), TEXT("2026-10-16T09:05:00"), TEXT("null")));
    TestFalse(TEXT("a job without one"), Plain->bHasMeta);
    TestEqual(TEXT("is never said"), Access.Changes(), BothSaid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaSyncedActorMetaNewestTest,
    "Looma.Generation.Meta.Actor.Newest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaSyncedActorMetaNewestTest::RunTest(const FString& Parameters)
{
    FLoomaSyncedActorMetaTestAccess Access;
    ALoomaSyncedActor* Seat = Access.Node(TEXT("chair"), Model(TEXT("gen-a")));

    // Two jobs that produced one asset. The list comes newest first, and the newest
    // is the one the backend's provenance names, so the older must not take over.
    Access.Listed(ListedJob(TEXT("new"), TEXT("gen-a"), TEXT("2026-10-16T10:00:00"), OtherMeta));
    Access.Listed(ListedJob(TEXT("old"), TEXT("gen-a"), TEXT("2026-10-15T10:00:00"), LevelMeta));
    TestEqual(TEXT("the newest job's meta"), Seat->MetaJson, FString(OtherMeta));

    Access.Receive(DoneEvent(TEXT("old"), TEXT("gen-a"), TEXT("2026-10-15T10:00:00"), LevelMeta));
    TestEqual(TEXT("an event for the older job changes nothing"), Seat->MetaJson, FString(OtherMeta));
    TestEqual(TEXT("and says nothing"), Access.Changes(), FString(TEXT("chair=")) + OtherMeta);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaSyncedActorMetaSoundTest,
    "Looma.Generation.Meta.Actor.Sound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaSyncedActorMetaSoundTest::RunTest(const FString& Parameters)
{
    FLoomaSyncedActorMetaTestAccess Access;
    Access.Listed(ListedJob(TEXT("model"), TEXT("gen-a"), TEXT("2026-10-16T09:00:00"), LevelMeta));
    Access.Listed(ListedJob(TEXT("sound"), TEXT("snd-a"), TEXT("2026-10-16T09:01:00"), OtherMeta));

    ALoomaSyncedActor* Speaker = Access.Node(TEXT("speaker"), Sound(TEXT("snd-a")));
    TestEqual(TEXT("a node with only a sound reports the sound's"), Speaker->MetaJson, FString(OtherMeta));

    ALoomaSyncedActor* Both = Access.Node(TEXT("both"), Model(TEXT("gen-a")) + TEXT(",") + Sound(TEXT("snd-a")));
    TestEqual(TEXT("a node with both reports the model's"), Both->MetaJson, FString(LevelMeta));

    // A model that was not generated still hides the sound's, as the web client has it.
    ALoomaSyncedActor* Catalog = Access.Node(TEXT("catalog"), Model(TEXT("chair_01")) + TEXT(",") + Sound(TEXT("snd-a")));
    TestFalse(TEXT("a catalog model with a generated sound"), Catalog->bHasMeta);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaSyncedActorMetaChangeTest,
    "Looma.Generation.Meta.Actor.Change",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaSyncedActorMetaChangeTest::RunTest(const FString& Parameters)
{
    FLoomaSyncedActorMetaTestAccess Access;
    Access.Listed(ListedJob(TEXT("j1"), TEXT("gen-a"), TEXT("2026-10-16T09:00:00"), LevelMeta));
    ALoomaSyncedActor* Seat = Access.Node(TEXT("chair"), Model(TEXT("gen-a")));
    TestTrue(TEXT("starts with one"), Seat->bHasMeta);

    // Swapped for a catalog model: the meta was the old asset's, so it goes.
    Access.Node(TEXT("chair"), Model(TEXT("chair_01")));
    TestFalse(TEXT("another asset drops it"), Seat->bHasMeta);
    TestTrue(TEXT("empty again"), Seat->MetaJson.IsEmpty());
    FString Value;
    TestFalse(TEXT("nothing to read"), Seat->GetGenerationMetaString(TEXT("unreal.environment"), Value));

    Access.Node(TEXT("chair"), FString());
    TestEqual(TEXT("clearing an empty one says nothing"),
        Access.Changes(), FString::Printf(TEXT("chair=%s | chair="), LevelMeta));

    // Placed before its job's own `done` reached us (another client was faster):
    // the event fills it in.
    Access.Node(TEXT("chair"), Model(TEXT("gen-c")));
    Access.Receive(DoneEvent(TEXT("j3"), TEXT("gen-c"), TEXT("2026-10-16T09:10:00"), OtherMeta));
    TestEqual(TEXT("a job finishing fills it in"), Seat->MetaJson, FString(OtherMeta));
    TestEqual(TEXT("and says so"), Access.Changes(),
        FString::Printf(TEXT("chair=%s | chair= | chair=%s"), LevelMeta, OtherMeta));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
