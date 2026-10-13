#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> ParseArray(const TCHAR* Json)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out);
        return Out;
    }

    FString TypeOf(const TSharedPtr<FJsonValue>& Value)
    {
        return Value->AsObject()->GetStringField(TEXT("type"));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveLinkComponentEditTest,
    "Looma.LiveLink.ComponentEdit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveLinkComponentEditTest::RunTest(const FString& Parameters)
{
    // A component edit is the node's whole array, so one this client does not
    // understand ("mystery") has to survive the edit untouched.
    const TArray<TSharedPtr<FJsonValue>> Before =
        ParseArray(TEXT(R"([{"type":"model","assetId":"a","url":"/static/a.glb"},{"type":"mystery","keep":7}])"));

    const TArray<TSharedPtr<FJsonValue>> Added = LoomaWithLiveLinkSubject(Before, TEXT("Xsens0"));
    TestEqual(TEXT("adding appends one component"), Added.Num(), 3);
    TestEqual(TEXT("the model keeps its url"), Added[0]->AsObject()->GetStringField(TEXT("url")), FString(TEXT("/static/a.glb")));
    TestEqual(TEXT("the unknown component keeps its payload"), Added[1]->AsObject()->GetNumberField(TEXT("keep")), 7.0);
    TestEqual(TEXT("the new component is livelink"), TypeOf(Added[2]), FString(TEXT("livelink")));
    TestEqual(TEXT("and names the subject"), Added[2]->AsObject()->GetStringField(TEXT("subject")), FString(TEXT("Xsens0")));
    TestEqual(TEXT("the parsed node reads the subject back"), LoomaParseComponents(&Added).LiveLinkSubject, FString(TEXT("Xsens0")));

    // A type occurs once per node: choosing again replaces, it does not add.
    const TArray<TSharedPtr<FJsonValue>> Changed = LoomaWithLiveLinkSubject(Added, TEXT("Actor1"));
    TestEqual(TEXT("changing keeps the count"), Changed.Num(), 3);
    TestEqual(TEXT("and names the new subject"), LoomaParseComponents(&Changed).LiveLinkSubject, FString(TEXT("Actor1")));

    const TArray<TSharedPtr<FJsonValue>> Removed = LoomaWithLiveLinkSubject(Changed, FString());
    TestEqual(TEXT("an empty subject removes the component"), Removed.Num(), 2);
    TestEqual(TEXT("and leaves the others in order"), TypeOf(Removed[1]), FString(TEXT("mystery")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveLinkComponentSourceTest,
    "Looma.LiveLink.ComponentSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveLinkComponentSourceTest::RunTest(const FString& Parameters)
{
    // Who has the subject (HAM-329): the backend's hub, in one of its streams.
    const TArray<TSharedPtr<FJsonValue>> Before =
        ParseArray(TEXT(R"([{"type":"model","assetId":"a"},{"type":"livelink","subject":"Actor0"}])"));
    TestFalse(TEXT("a component with no source is a client's"), LoomaParseComponents(&Before).IsLiveLinkFromHub());

    const TArray<TSharedPtr<FJsonValue>> Hubbed =
        LoomaWithLiveLinkSubject(Before, TEXT("Actor0"), LoomaLiveLinkHubSource, TEXT("studio-b"));
    const TSharedPtr<FJsonObject> Written = Hubbed.Last()->AsObject();
    TestEqual(TEXT("it says the hub has it"), Written->GetStringField(TEXT("source")), FString(TEXT("hub")));
    TestEqual(TEXT("and in which stream"), Written->GetStringField(TEXT("stream")), FString(TEXT("studio-b")));
    const FLoomaNodeComponents Parsed = LoomaParseComponents(&Hubbed);
    TestTrue(TEXT("and reads back as the hub's"), Parsed.IsLiveLinkFromHub());
    TestEqual(TEXT("with its stream"), Parsed.LiveLinkStream, FString(TEXT("studio-b")));
    TestTrue(TEXT("a different source is a different component"), Parsed != LoomaParseComponents(&Before));

    // Choosing a client's subject writes the component whole: no source, no stream.
    const TArray<TSharedPtr<FJsonValue>> Back = LoomaWithLiveLinkSubject(Hubbed, TEXT("Actor0"));
    TestFalse(TEXT("a client's subject has no source"), Back.Last()->AsObject()->HasField(TEXT("source")));
    TestFalse(TEXT("nor a stream"), Back.Last()->AsObject()->HasField(TEXT("stream")));
    TestFalse(TEXT("and is no longer the hub's"), LoomaParseComponents(&Back).IsLiveLinkFromHub());

    // A stream only means something with the hub as the source.
    const TArray<TSharedPtr<FJsonValue>> Loose = LoomaWithLiveLinkSubject(Before, TEXT("Actor0"), FString(), TEXT("studio-b"));
    TestFalse(TEXT("a stream with no source is not written"), Loose.Last()->AsObject()->HasField(TEXT("stream")));

    const TArray<TSharedPtr<FJsonValue>> AnyStream = ParseArray(
        TEXT(R"([{"type":"livelink","subject":"Actor0","source":"hub"}])"));
    TestTrue(TEXT("the hub with no stream named is still the hub"), LoomaParseComponents(&AnyStream).IsLiveLinkFromHub());
    const TArray<TSharedPtr<FJsonValue>> Nobody = ParseArray(TEXT(R"([{"type":"livelink","subject":"","source":"hub"}])"));
    TestFalse(TEXT("an empty subject is nobody's, whatever the source"), LoomaParseComponents(&Nobody).IsLiveLinkFromHub());
    return true;
}

/** Feeds wire frames to a subsystem that never opened a socket. */
struct FLoomaLivePoseTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};

    void Receive(const FString& Json) { Sync->OnRawMessage(Json); }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLivePoseReceiveTest,
    "Looma.LiveLink.ReceivePose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLivePoseReceiveTest::RunTest(const FString& Parameters)
{
    FLoomaLivePoseTestAccess Access;
    FLoomaLivePose Pose;
    TestFalse(TEXT("nothing before a frame arrives"), Access.Sync->GetLivePose(TEXT("n1"), Pose));

    FString Rot;
    for (int32 Bone = 0; Bone < 22; ++Bone)
    {
        Rot += FString::Printf(TEXT("%s[0,0,%s]"), Bone > 0 ? TEXT(",") : TEXT(""), Bone == 1 ? TEXT("1,0") : TEXT("0,1"));
    }
    Access.Receive(FString::Printf(TEXT("{\"type\":\"pose\",\"node\":\"n1\",\"hips\":[0.1,1.0,-0.4],\"rot\":[%s]}"), *Rot));
    TestTrue(TEXT("a frame is kept"), Access.Sync->GetLivePose(TEXT("n1"), Pose));
    TestEqual(TEXT("with all its rotations"), Pose.Rotations.Num(), 22);
    TestEqual(TEXT("hips as sent: left, up, forward"), Pose.Hips, FVector(0.1, 1.0, -0.4));
    TestEqual(TEXT("a quaternion as x, y, z, w"), Pose.Rotations[1], FQuat(0, 0, 1, 0));
    TestFalse(TEXT("and only for its own node"), Access.Sync->GetLivePose(TEXT("n2"), Pose));

    Access.Receive(TEXT(R"({"type":"pose","node":"n3","hips":[0,1],"rot":[[0,0,0,1]]})"));
    TestFalse(TEXT("a malformed frame is dropped"), Access.Sync->GetLivePose(TEXT("n3"), Pose));

    // The hub's own poses carry when each stands for and its tick (HAM-328):
    // additive fields, read past.
    Access.Receive(FString::Printf(
        TEXT("{\"type\":\"pose\",\"node\":\"n4\",\"hips\":[0,1,0],\"rot\":[%s],\"t\":8812.4518,\"seq\":2291}"), *Rot));
    TestTrue(TEXT("a pose from the hub, with t and seq, is kept too"), Access.Sync->GetLivePose(TEXT("n4"), Pose));

    TestTrue(TEXT("the pose to show is there too"), Access.Sync->SampleLivePose(TEXT("n1"), Pose));
    TestEqual(TEXT("and with one frame received it is that frame"), Pose.Hips, FVector(0.1, 1.0, -0.4));
    double Delay = 0.0, Jitter = 0.0;
    TestTrue(TEXT("its timing can be asked for"), Access.Sync->GetLivePoseTiming(TEXT("n1"), Delay, Jitter));
    TestFalse(TEXT("but not that of a node that receives nothing"), Access.Sync->GetLivePoseTiming(TEXT("n2"), Delay, Jitter));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaHubLiveSubjectsTest,
    "Looma.LiveLink.HubSubjects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaHubLiveSubjectsTest::RunTest(const FString& Parameters)
{
    FLoomaLivePoseTestAccess Access;
    TMap<FString, TArray<FString>> Streams;
    TestFalse(TEXT("nothing before the hub says anything"), Access.Sync->GetHubLiveSubjects(Streams));

    Access.Receive(TEXT(R"({"type":"liveSubjects","clientId":"ue-2","subjects":["Actor0"]})"));
    TestFalse(TEXT("another client's subjects are not the hub's"), Access.Sync->GetHubLiveSubjects(Streams));

    Access.Receive(TEXT(R"({"type":"liveSubjects","clientId":"hub","subjects":["Actor0","Actor1"],"streams":{"moverse":["Actor0","Actor1"],"studio-b":["Actor0",""]}})"));
    TestTrue(TEXT("the hub's are kept"), Access.Sync->GetHubLiveSubjects(Streams));
    TestEqual(TEXT("by stream"), Streams.Num(), 2);
    TestEqual(TEXT("each with its performers"), Streams.FindRef(TEXT("moverse")).Num(), 2);
    TestEqual(TEXT("and no empty names"), Streams.FindRef(TEXT("studio-b")), TArray<FString>{TEXT("Actor0")});

    Access.Receive(TEXT(R"({"type":"liveSubjects","clientId":"hub","subjects":["Actor2"]})"));
    TestTrue(TEXT("a hub that names no streams"), Access.Sync->GetHubLiveSubjects(Streams));
    TestEqual(TEXT("reads as one with no name"), Streams.FindRef(FString()), TArray<FString>{TEXT("Actor2")});
    TestEqual(TEXT("replacing what it said before"), Streams.Num(), 1);
    return true;
}

namespace
{
    /** A wire pose whose every bone is turned `Angle` radians about up, with the hips that far forward. */
    FLoomaLivePose Turned(double Angle)
    {
        FLoomaLivePose Pose;
        Pose.Hips = FVector(0, 1, Angle);
        Pose.Rotations.Init(FQuat(FVector(0, 1, 0), Angle), 22);
        return Pose;
    }

    /** How far a sampled pose has turned, read back from a bone. */
    double AngleOf(const FLoomaLivePose& Pose)
    {
        return Pose.Rotations[5].AngularDistance(FQuat::Identity);
    }

    struct FPlayback
    {
        /** The biggest and smallest step between rendered frames, as a share of the step a steady movement makes. */
        double Fastest = 0.0;
        double Slowest = TNumericLimits<double>::Max();
        double Delay = 0.0;
    };

    /**
     * A bone turning steadily, sent `Rate` times a second, each pose arriving up
     * to `Late` seconds after it was sent; watched at 60 frames a second, either
     * through the buffer or by taking the newest pose.
     */
    FPlayback Watch(double Rate, double Late, bool bBuffered, int32 Seed)
    {
        const double Speed = 1.0; // radians a second
        const double Seconds = 6.0;
        const double Frame = 1.0 / 60.0;
        FRandomStream Random(Seed);
        TArray<TPair<double, double>> Arrivals; // when it arrives, the angle it carries
        double LastArrival = 0.0;
        for (double Sent = 0.0; Sent < Seconds; Sent += 1.0 / Rate)
        {
            // A socket delays poses but keeps their order.
            LastArrival = FMath::Max(LastArrival, Sent + Random.FRand() * Late);
            Arrivals.Emplace(LastArrival, Sent * Speed);
        }
        FLoomaLivePoseBuffer Buffer;
        FPlayback Out;
        int32 Next = 0;
        double Last = 0.0;
        for (double Now = 0.0; Now < Seconds; Now += Frame)
        {
            // A client reads its socket once a rendered frame.
            for (; Next < Arrivals.Num() && Arrivals[Next].Key <= Now; ++Next)
            {
                Buffer.Push(Turned(Arrivals[Next].Value), Now);
            }
            FLoomaLivePose Shown;
            if (!(bBuffered ? Buffer.Sample(Now, Shown) : Buffer.Newest(Now, Shown)))
            {
                continue;
            }
            const double Angle = Shown.Hips.Z; // travels with the bones, and does not wrap as an angle does
            if (Now > 3.0) // once the buffer has the measure of the arrivals
            {
                const double Step = (Angle - Last) / (Speed * Frame);
                Out.Fastest = FMath::Max(Out.Fastest, Step);
                Out.Slowest = FMath::Min(Out.Slowest, Step);
            }
            Last = Angle;
        }
        Out.Delay = Buffer.GetDelay();
        return Out;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLivePoseBufferTest,
    "Looma.LiveLink.PoseBuffer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLivePoseBufferTest::RunTest(const FString& Parameters)
{
    FLoomaLivePose Pose;
    FLoomaLivePoseBuffer Empty;
    TestFalse(TEXT("nothing before a pose arrives"), Empty.Sample(0.0, Pose));

    // Between two poses: the character is shown part of the way from one to the next.
    FLoomaLivePoseBuffer Two;
    Two.Push(Turned(0.0), 10.0);
    TestTrue(TEXT("one pose is shown as it is"), Two.Sample(10.0, Pose) && FMath::IsNearlyZero(AngleOf(Pose), 1e-6));
    Two.Push(Turned(0.4), 10.1);
    TestTrue(TEXT("with two poses the delay is the gap between them"), FMath::IsNearlyEqual(Two.GetDelay(), 0.1, 1e-6));
    Two.Sample(10.15, Pose);
    TestTrue(TEXT("half a gap on, the bone is half way"), FMath::IsNearlyEqual(AngleOf(Pose), 0.2, 1e-4));
    TestTrue(TEXT("and so are the hips"), FMath::IsNearlyEqual(Pose.Hips.Z, 0.2, 1e-4));
    Two.Sample(10.5, Pose);
    TestTrue(TEXT("with nothing newer, the newest is held"), FMath::IsNearlyEqual(AngleOf(Pose), 0.4, 1e-6));
    TestTrue(TEXT("the newest pose is still to be had as it came"), Two.Newest(10.15, Pose) && FMath::IsNearlyEqual(AngleOf(Pose), 0.4, 1e-6));
    TestFalse(TEXT("a node goes stale"), Two.Sample(10.1 + FLoomaLivePoseBuffer::StaleSeconds + 0.5, Pose));
    Two.Push(Turned(2.0), 20.0);
    TestTrue(TEXT("after a silence the next pose is a new start"),
             Two.Sample(20.0, Pose) && FMath::IsNearlyEqual(AngleOf(Pose), 2.0, 1e-6) && Two.GetDelay() == 0.0);

    // Steady arrivals: an even movement, about one gap behind.
    const FPlayback Steady = Watch(30.0, 0.0, true, 1);
    AddInfo(FString::Printf(TEXT("steady: step %.2f to %.2f, delay %.0f ms"), Steady.Slowest, Steady.Fastest, Steady.Delay * 1000));
    TestTrue(TEXT("steady poses play back evenly"), Steady.Slowest > 0.9 && Steady.Fastest < 1.1);
    TestTrue(TEXT("about two rendered frames behind"), Steady.Delay > 0.03 && Steady.Delay < 0.06);

    // Uneven arrivals, each pose up to 40 ms late: taking the newest pose stops
    // and lurches, the buffer does not.
    for (int32 Seed = 1; Seed <= 5; ++Seed)
    {
        const FPlayback Chasing = Watch(30.0, 0.04, false, Seed);
        const FPlayback Buffered = Watch(30.0, 0.04, true, Seed);
        AddInfo(FString::Printf(TEXT("uneven %d: newest %.2f to %.2f; buffered %.2f to %.2f, delay %.0f ms"), Seed,
                                Chasing.Slowest, Chasing.Fastest, Buffered.Slowest, Buffered.Fastest, Buffered.Delay * 1000));
        TestTrue(TEXT("the newest pose stands still on some frames and jumps on others"), Chasing.Slowest < 0.1 && Chasing.Fastest > 2.5);
        TestTrue(TEXT("the buffer never stands still"), Buffered.Slowest > 0.5);
        TestTrue(TEXT("and never lurches"), Buffered.Fastest < 1.6);
        TestTrue(TEXT("for a delay under a tenth of a second"), Buffered.Delay < 0.1);
    }

    // A crowd sharing the budget of the hub: a few poses a second each.
    const FPlayback Sparse = Watch(6.0, 0.02, true, 3);
    AddInfo(FString::Printf(TEXT("sparse: step %.2f to %.2f, delay %.0f ms"), Sparse.Slowest, Sparse.Fastest, Sparse.Delay * 1000));
    TestTrue(TEXT("a few poses a second still play back evenly"), Sparse.Slowest > 0.7 && Sparse.Fastest < 1.3);
    TestTrue(TEXT("no further behind than the longest delay"), Sparse.Delay <= FLoomaLivePoseBuffer::LongestDelay);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
