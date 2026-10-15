#include "LoomaGenerationLibrary.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSubmitGenerationAction.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "LoomaGenerationHandle.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tests/LoomaGenerationTestObserver.h"
#include "UObject/StrongObjectPtr.h"

/**
 * A generation job's `meta` (looma-xr-asset-demo docs/unreal-sync.md, "The
 * submitter's context"): read off both transports, kept once known although a job's
 * earliest events lack it, sent as an object, refused before any request when it is
 * not one, and built and read by dotted path from Blueprint.
 */

namespace
{
    TSharedPtr<FJsonObject> ParseObject(const FString& Json)
    {
        TSharedPtr<FJsonObject> Out;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out);
        return Out;
    }

    FLoomaGenerationJob ParseJob(const TCHAR* Json)
    {
        return LoomaParseGenerationJob(ParseObject(Json));
    }

    /** A `generation` frame as the hub sends it; Meta is JSON text, "null" for none. */
    FString GenerationEvent(const TCHAR* JobId, const TCHAR* State, const TCHAR* Meta)
    {
        return FString::Printf(
            TEXT(R"({"type":"generation","job":{"jobId":"%s","prompt":"a chair","state":"%s","progress":0,"meta":%s}})"),
            JobId, State, Meta);
    }

    FString WriteBody(const TSharedRef<FJsonObject>& Body)
    {
        FString Text;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
        FJsonSerializer::Serialize(Body, Writer);
        return Text;
    }

    const TCHAR* const EnvironmentMeta = TEXT(R"({"unreal":{"environment":"LVL_Demo"}})");
}

/** Feeds wire frames to a subsystem that never opened a socket. */
struct FLoomaGenerationMetaTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};

    void Receive(const FString& Json) { Sync->OnRawMessage(Json); }

    FLoomaGenerationJob Cached(const TCHAR* JobId) const
    {
        FLoomaGenerationJob Job;
        Sync->GetGenerationJob(JobId, Job);
        return Job;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGenerationMetaParseTest,
    "Looma.Generation.Meta.Parse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaGenerationMetaParseTest::RunTest(const FString& Parameters)
{
    // The event's camelCase job. The meta comes back compact, with its groups, its
    // key order and its keys exactly as sent, and its numbers as the browser writes them.
    const FLoomaGenerationJob Event = ParseJob(TEXT(R"({"jobId":"j1","state":"queued","meta":
        {"unreal": {"environment": "LVL_Demo", "floor": 2, "scale": 0.1},
         "web": {"tags": ["a", "b"], "lit": true, "snake_key": null, "camelKey": {}}}})"));
    TestTrue(TEXT("an object meta is kept"), Event.bHasMeta);
    TestEqual(TEXT("as compact JSON text"), Event.MetaJson, FString(TEXT(
        R"({"unreal":{"environment":"LVL_Demo","floor":2,"scale":0.1},"web":{"tags":["a","b"],"lit":true,"snake_key":null,"camelKey":{}}})")));

    // The REST hydrate's snake_case job. `meta` is the same key in both, and the keys
    // inside it are never re-cased either way.
    const FLoomaGenerationJob Rest = ParseJob(TEXT(R"({"job_id":"j2","state":"done","asset_id":"gen-1",
        "meta":{"unreal":{"environment":"LVL_Demo","someKey":"x"}}})"));
    TestEqual(TEXT("the REST job is read"), Rest.JobId, FString(TEXT("j2")));
    TestTrue(TEXT("with its meta"), Rest.bHasMeta);
    TestEqual(TEXT("keys untouched"), Rest.MetaJson,
        FString(TEXT(R"({"unreal":{"environment":"LVL_Demo","someKey":"x"}})")));

    const FLoomaGenerationJob Empty = ParseJob(TEXT(R"({"jobId":"j3","state":"queued","meta":{}})"));
    TestTrue(TEXT("an empty object is still a meta"), Empty.bHasMeta);
    TestEqual(TEXT("written as {}"), Empty.MetaJson, FString(TEXT("{}")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGenerationMetaNullTest,
    "Looma.Generation.Meta.Null",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaGenerationMetaNullTest::RunTest(const FString& Parameters)
{
    // A job's earliest events, and every event of a job submitted without a meta.
    const FLoomaGenerationJob Null = ParseJob(TEXT(R"({"jobId":"j1","state":"queued","meta":null})"));
    TestEqual(TEXT("the job is still read"), Null.JobId, FString(TEXT("j1")));
    TestFalse(TEXT("a null meta is none"), Null.bHasMeta);
    TestTrue(TEXT("with no text"), Null.MetaJson.IsEmpty());

    const FLoomaGenerationJob Absent = ParseJob(TEXT(R"({"jobId":"j1","state":"queued"})"));
    TestFalse(TEXT("an older backend sends no meta key at all"), Absent.bHasMeta);

    // The backend never sends these, but a string holding JSON is still not an object.
    const FLoomaGenerationJob Text = ParseJob(TEXT(R"({"jobId":"j1","state":"queued","meta":"{\"a\":1}"})"));
    TestFalse(TEXT("a string is not a meta"), Text.bHasMeta);
    const FLoomaGenerationJob List = ParseJob(TEXT(R"({"jobId":"j1","state":"queued","meta":[1]})"));
    TestFalse(TEXT("nor is a list"), List.bHasMeta);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGenerationMetaMergeTest,
    "Looma.Generation.Meta.Merge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaGenerationMetaMergeTest::RunTest(const FString& Parameters)
{
    FLoomaGenerationMetaTestAccess Access;
    TStrongObjectPtr<ULoomaGenerationTestObserver> HubWide{NewObject<ULoomaGenerationTestObserver>()};
    Access.Sync->OnGenerationJobUpdated.AddDynamic(HubWide.Get(), &ULoomaGenerationTestObserver::ObserveJob);

    // Another client's job, watched from here: the backend announces it with
    // `meta: null`, re-announces it with the meta, and later states carry it again
    // or not. Once learned it stays, everywhere a job is read.
    ULoomaGenerationHandle* Handle = Access.Sync->GetGenerationHandle(TEXT("j1"));
    TStrongObjectPtr<ULoomaGenerationTestObserver> PerJob{NewObject<ULoomaGenerationTestObserver>()};
    Handle->OnUpdated.AddDynamic(PerJob.Get(), &ULoomaGenerationTestObserver::ObserveJob);

    Access.Receive(GenerationEvent(TEXT("j1"), TEXT("queued"), TEXT("null")));
    TestFalse(TEXT("the first event has no meta"), Access.Cached(TEXT("j1")).bHasMeta);

    Access.Receive(GenerationEvent(TEXT("j1"), TEXT("queued"), EnvironmentMeta));
    TestTrue(TEXT("the re-announce brings it"), Access.Cached(TEXT("j1")).bHasMeta);

    Access.Receive(GenerationEvent(TEXT("j1"), TEXT("running"), TEXT("null")));
    const FLoomaGenerationJob Later = Access.Cached(TEXT("j1"));
    TestTrue(TEXT("a later event without it keeps it"), Later.bHasMeta);
    TestEqual(TEXT("unchanged"), Later.MetaJson, FString(EnvironmentMeta));
    TestEqual(TEXT("while the rest of the snapshot moves on"), Later.State, ELoomaJobState::Running);

    auto CheckDelivered = [this](const TCHAR* Who, const TArray<FLoomaGenerationJob>& Jobs)
    {
        if (!TestEqual(FString::Printf(TEXT("%s: every event delivered"), Who), Jobs.Num(), 3))
        {
            return;
        }
        TestFalse(FString::Printf(TEXT("%s: not on the first"), Who), Jobs[0].bHasMeta);
        TestTrue(FString::Printf(TEXT("%s: on the second"), Who), Jobs[1].bHasMeta);
        TestTrue(FString::Printf(TEXT("%s: and on the third"), Who), Jobs[2].bHasMeta);
        TestEqual(FString::Printf(TEXT("%s: the same text"), Who), Jobs[2].MetaJson, FString(EnvironmentMeta));
    };
    CheckDelivered(TEXT("hub-wide"), HubWide->Jobs);
    CheckDelivered(TEXT("handle"), PerJob->Jobs);
    TestEqual(TEXT("the handle's snapshot has it"), Handle->Job.MetaJson, FString(EnvironmentMeta));

    // Our own job: `Submit Generation` records the meta it sent as soon as it has the
    // job id, so the submitter never sees the job without it.
    ULoomaGenerationHandle* Mine = Access.Sync->GetGenerationHandle(TEXT("j2"));
    Access.Sync->NoteGenerationMeta(TEXT("j2"), EnvironmentMeta);
    TestTrue(TEXT("the submitter's handle has it before any event"), Mine->Job.bHasMeta);
    Access.Receive(GenerationEvent(TEXT("j2"), TEXT("queued"), TEXT("null")));
    TestTrue(TEXT("and the first event does not take it away"), Mine->Job.bHasMeta);
    TestTrue(TEXT("from the cache either"), Access.Cached(TEXT("j2")).bHasMeta);

    // The first event can beat the POST response; the response then patches the cache.
    Access.Receive(GenerationEvent(TEXT("j3"), TEXT("queued"), TEXT("null")));
    Access.Sync->NoteGenerationMeta(TEXT("j3"), EnvironmentMeta);
    TestEqual(TEXT("a job cached without it is patched"), Access.Cached(TEXT("j3")).MetaJson, FString(EnvironmentMeta));

    Access.Receive(GenerationEvent(TEXT("j4"), TEXT("queued"), TEXT("null")));
    Access.Receive(GenerationEvent(TEXT("j4"), TEXT("running"), TEXT("null")));
    TestFalse(TEXT("a job sent without one never has one"), Access.Cached(TEXT("j4")).bHasMeta);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGenerationMetaSubmitBodyTest,
    "Looma.Generation.Meta.SubmitBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaGenerationMetaSubmitBodyTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Meta;
    FString Error;
    TestTrue(TEXT("a JSON object is accepted"), LoomaReadGenerationMeta(
        TEXT(R"(  {"unreal": {"environment": "LVL_Demo", "scale": 0.1}}  )"), Meta, Error));
    TestTrue(TEXT("with no error"), Error.IsEmpty());

    const TSharedRef<FJsonObject> Body = ULoomaSubmitGenerationAction::MakeRequestBody(
        TEXT("a chair"), FTransform::Identity, /*bSuggestTransform=*/false, 3, Meta);
    const FString Text = WriteBody(Body);

    // Read back the way the backend will. A string here would be a 422.
    const TSharedPtr<FJsonObject> Sent = ParseObject(Text);
    if (!TestTrue(TEXT("the body is JSON"), Sent.IsValid()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Field = Sent->TryGetField(TEXT("meta"));
    if (!TestTrue(TEXT("the body has a meta"), Field.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("sent as an object"), Field->Type == EJson::Object);
    TestFalse(TEXT("never as a string"), Field->Type == EJson::String);
    TestTrue(TEXT("written compact in the body"),
        Text.Contains(TEXT(R"("meta":{"unreal":{"environment":"LVL_Demo","scale":0.1}})")));
    TestEqual(TEXT("beside the prompt"), Sent->GetStringField(TEXT("prompt")), FString(TEXT("a chair")));

    const TSharedRef<FJsonObject> Bare = ULoomaSubmitGenerationAction::MakeRequestBody(
        TEXT("a chair"), FTransform::Identity, false, 3, nullptr);
    TestFalse(TEXT("no meta, no field"), Bare->HasField(TEXT("meta")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGenerationMetaRefuseTest,
    "Looma.Generation.Meta.RefuseNonObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaGenerationMetaRefuseTest::RunTest(const FString& Parameters)
{
    // The backend refuses each of these. Here they fail before any request.
    for (const TCHAR* Bad : { TEXT(R"("LVL_Demo")"), TEXT("[1,2]"), TEXT("3"), TEXT("true"), TEXT("null"),
        TEXT(R"({"unreal":)"), TEXT("unreal.environment=LVL_Demo"), TEXT(R"({"big":1e999})") })
    {
        TSharedPtr<FJsonObject> Meta;
        FString Error;
        TestFalse(FString::Printf(TEXT("%s is refused"), Bad), LoomaReadGenerationMeta(Bad, Meta, Error));
        TestFalse(FString::Printf(TEXT("%s leaves nothing to send"), Bad), Meta.IsValid());
        TestTrue(FString::Printf(TEXT("%s gets a reason naming MetaJson: %s"), Bad, *Error), Error.Contains(TEXT("MetaJson")));
    }

    for (const TCHAR* Blank : { TEXT(""), TEXT("  \n") })
    {
        TSharedPtr<FJsonObject> Meta;
        FString Error;
        TestTrue(TEXT("blank is no meta, not an error"), LoomaReadGenerationMeta(Blank, Meta, Error));
        TestFalse(TEXT("and nothing to send"), Meta.IsValid());
    }

    // The node itself. With no world to send through, how far it gets says where the
    // check sits: a bad meta fails on the meta, synchronously, before the node even
    // looks for the subsystem it would send with.
    TStrongObjectPtr<ULoomaGenerationTestObserver> Observer{NewObject<ULoomaGenerationTestObserver>()};
    TStrongObjectPtr<ULoomaSubmitGenerationAction> Refused{ULoomaSubmitGenerationAction::SubmitGeneration(
        nullptr, TEXT("a chair"), FTransform::Identity, false, 3, TEXT("[1,2]"))};
    Refused->OnFailed.AddDynamic(Observer.Get(), &ULoomaGenerationTestObserver::ObserveError);
    Refused->Activate();
    if (TestEqual(TEXT("the node fails at once"), Observer->Errors.Num(), 1))
    {
        TestTrue(TEXT("on the meta"), Observer->Errors[0].Contains(TEXT("MetaJson")));
    }

    Observer->Errors.Reset();
    TStrongObjectPtr<ULoomaSubmitGenerationAction> Accepted{ULoomaSubmitGenerationAction::SubmitGeneration(
        nullptr, TEXT("a chair"), FTransform::Identity, false, 3, EnvironmentMeta)};
    Accepted->OnFailed.AddDynamic(Observer.Get(), &ULoomaGenerationTestObserver::ObserveError);
    Accepted->Activate();
    if (TestEqual(TEXT("a good meta gets past the check"), Observer->Errors.Num(), 1))
    {
        TestEqual(TEXT("as far as the missing subsystem"), Observer->Errors[0],
            FString(TEXT("LoomaSceneSync subsystem unavailable")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGenerationMetaPathTest,
    "Looma.Generation.Meta.Path",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaGenerationMetaPathTest::RunTest(const FString& Parameters)
{
    // Building: dotted paths become groups, in the map's order.
    TMap<FString, FString> Values;
    Values.Add(TEXT("unreal.environment"), TEXT("LVL_Demo"));
    Values.Add(TEXT("unreal.level"), TEXT("Persistent"));
    Values.Add(TEXT("note"), TEXT("3"));
    FString MetaJson;
    TestTrue(TEXT("a map of paths builds"), ULoomaGenerationLibrary::MakeGenerationMeta(Values, MetaJson));
    TestEqual(TEXT("into groups, every value a string"), MetaJson,
        FString(TEXT(R"({"unreal":{"environment":"LVL_Demo","level":"Persistent"},"note":"3"})")));
    const FString Built = MetaJson;

    TestTrue(TEXT("an empty map builds"), ULoomaGenerationLibrary::MakeGenerationMeta({}, MetaJson));
    TestTrue(TEXT("into no meta at all"), MetaJson.IsEmpty());

    AddExpectedMessage(TEXT("Make Generation Meta"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
    TMap<FString, FString> ValueThenGroup;
    ValueThenGroup.Add(TEXT("unreal"), TEXT("x"));
    ValueThenGroup.Add(TEXT("unreal.environment"), TEXT("LVL_Demo"));
    TestFalse(TEXT("a value then a group under it is refused"),
        ULoomaGenerationLibrary::MakeGenerationMeta(ValueThenGroup, MetaJson));
    TestTrue(TEXT("leaving nothing"), MetaJson.IsEmpty());
    TMap<FString, FString> GroupThenValue;
    GroupThenValue.Add(TEXT("unreal.environment"), TEXT("LVL_Demo"));
    GroupThenValue.Add(TEXT("unreal"), TEXT("x"));
    TestFalse(TEXT("and the other way round"), ULoomaGenerationLibrary::MakeGenerationMeta(GroupThenValue, MetaJson));
    TMap<FString, FString> Gap;
    Gap.Add(TEXT("unreal..environment"), TEXT("LVL_Demo"));
    TestFalse(TEXT("a path with an empty part is refused"), ULoomaGenerationLibrary::MakeGenerationMeta(Gap, MetaJson));

    // Reading.
    const FLoomaGenerationJob Job = ParseJob(TEXT(R"({"jobId":"j1","state":"done","meta":{"unreal":
        {"environment":"LVL_Demo","floor":2,"scale":0.1,"lit":true,"tags":["a"],"none":null}}})"));
    FString Value;
    TestTrue(TEXT("a string is found"), ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal.environment"), Value));
    TestEqual(TEXT("as itself"), Value, FString(TEXT("LVL_Demo")));
    ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal.floor"), Value);
    TestEqual(TEXT("an integer as its JSON text"), Value, FString(TEXT("2")));
    ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal.scale"), Value);
    TestEqual(TEXT("a fraction in its shortest form"), Value, FString(TEXT("0.1")));
    ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal.lit"), Value);
    TestEqual(TEXT("a boolean as true or false"), Value, FString(TEXT("true")));
    ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal.tags"), Value);
    TestEqual(TEXT("a list as compact JSON"), Value, FString(TEXT(R"(["a"])")));
    ULoomaGenerationLibrary::GetGenerationMetaString(Job, TEXT("unreal"), Value);
    TestEqual(TEXT("a group as compact JSON"), Value,
        FString(TEXT(R"({"environment":"LVL_Demo","floor":2,"scale":0.1,"lit":true,"tags":["a"],"none":null})")));

    for (const TCHAR* Missing : { TEXT("unreal.none"), TEXT("unreal.missing"), TEXT("web"),
        TEXT("unreal.environment.deeper"), TEXT("unreal..environment"), TEXT("") })
    {
        TestFalse(FString::Printf(TEXT("'%s' is not found"), Missing),
            ULoomaGenerationLibrary::GetGenerationMetaString(Job, Missing, Value));
        TestTrue(FString::Printf(TEXT("'%s' leaves the value empty"), Missing), Value.IsEmpty());
    }
    TestFalse(TEXT("a job without a meta has nothing"), ULoomaGenerationLibrary::GetGenerationMetaString(
        ParseJob(TEXT(R"({"jobId":"j2","state":"done","meta":null})")), TEXT("unreal.environment"), Value));

    // And the round trip a Blueprint makes: build it, send it, read it back off a job
    // carrying what was sent, as the backend echoes it.
    TSharedPtr<FJsonObject> Sent;
    FString Error;
    TestTrue(TEXT("what Make builds is a meta Submit accepts"), LoomaReadGenerationMeta(Built, Sent, Error));
    const TSharedPtr<FJsonObject> Body = ParseObject(WriteBody(
        ULoomaSubmitGenerationAction::MakeRequestBody(TEXT("a chair"), FTransform::Identity, false, 3, Sent)));
    const TSharedRef<FJsonObject> EchoedJob = MakeShared<FJsonObject>();
    EchoedJob->SetStringField(TEXT("jobId"), TEXT("j3"));
    EchoedJob->SetField(TEXT("meta"), Body->TryGetField(TEXT("meta")));
    const FLoomaGenerationJob Echoed = LoomaParseGenerationJob(EchoedJob);
    TestEqual(TEXT("and comes back as the same text"), Echoed.MetaJson, Built);
    TestTrue(TEXT("whose values read back"), ULoomaGenerationLibrary::GetGenerationMetaString(
        Echoed, TEXT("unreal.environment"), Value));
    TestEqual(TEXT("unchanged"), Value, FString(TEXT("LVL_Demo")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
