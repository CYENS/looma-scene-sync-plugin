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

/** Feeds wire frames to a subsystem that never opened a socket. */
struct FLoomaLivePoseTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};

    void Receive(const FString& Json) { Sync->OnRawMessage(Json); }
    void Age(const TCHAR* Node, double Seconds) { Sync->LivePoses.FindChecked(Node).ReceivedAt -= Seconds; }
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

    Access.Age(TEXT("n1"), ULoomaSceneSyncSubsystem::LivePoseStaleSeconds + 0.5);
    TestFalse(TEXT("a frame goes stale"), Access.Sync->GetLivePose(TEXT("n1"), Pose));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
