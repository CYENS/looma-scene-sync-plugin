#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/AudioComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> ParseArray(const FString& Json)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Values);
        return Values;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaComponentEnabledParseTest,
    "Looma.Components.Enabled.Parse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaComponentEnabledParseTest::RunTest(const FString& Parameters)
{
    // The format switches off only a literal false. Unreal's JSON bool reader also
    // accepts numbers and strings, so those need checking as well as old documents.
    const TCHAR* Fields[] = { TEXT(""), TEXT(",\"enabled\":true"), TEXT(",\"enabled\":false"),
        TEXT(",\"enabled\":0"), TEXT(",\"enabled\":\"false\""), TEXT(",\"enabled\":null") };
    for (const TCHAR* Field : Fields)
    {
        const TArray<TSharedPtr<FJsonValue>> Values = ParseArray(FString::Printf(TEXT(R"([
            {"type":"model","assetId":"chair"%s}, {"type":"mesh"%s},
            {"type":"material"%s}, {"type":"light"%s}, {"type":"audio","assetId":"sound"%s},
            {"type":"skybox","assetId":"sky"%s}, {"type":"animation","clip":"Walk"%s}, {"type":"livelink","subject":"Actor0","source":"hub"%s}
        ])"), Field, Field, Field, Field, Field, Field, Field, Field));
        const bool bActive = FString(Field) != TEXT(",\"enabled\":false");
        const FLoomaNodeComponents Parsed = LoomaParseComponents(&Values);
        const FString Label = FString::Printf(TEXT("field '%s': "), Field);
        TestEqual(Label + TEXT("model"), Parsed.bHasModel, bActive);
        TestEqual(Label + TEXT("mesh"), Parsed.bHasMesh, bActive);
        TestEqual(Label + TEXT("material"), Parsed.bHasMaterial, bActive);
        TestEqual(Label + TEXT("light"), Parsed.bHasLight, bActive);
        TestEqual(Label + TEXT("audio"), Parsed.bHasAudio, bActive);
        TestEqual(Label + TEXT("skybox"), Parsed.bHasSkybox, bActive);
        TestEqual(Label + TEXT("animation"), Parsed.bHasAnimation, bActive);
        TestEqual(Label + TEXT("Live Link subject"), !Parsed.LiveLinkSubject.IsEmpty(), bActive);
        TestEqual(Label + TEXT("hub driver"), Parsed.IsLiveLinkFromHub(), bActive);
        TestEqual(TEXT("parsing keeps every wire component"), Values.Num(), 8);
    }

    // A later Live Link edit must keep the disabled model's fields, even though it
    // currently contributes no rendering spec. Edits use the retained wire array.
    const TArray<TSharedPtr<FJsonValue>> Wire = ParseArray(
        TEXT(R"([{"type":"model","assetId":"chair","enabled":false,"custom":7}])"));
    const TArray<TSharedPtr<FJsonValue>> Edited = LoomaWithLiveLinkSubject(Wire, TEXT("Actor0"));
    TestFalse(TEXT("an unrelated edit keeps the switch off"), Edited[0]->AsObject()->GetBoolField(TEXT("enabled")));
    TestEqual(TEXT("and keeps the component's payload"), Edited[0]->AsObject()->GetNumberField(TEXT("custom")), 7.0);
    return true;
}

/** Real scene patches, without restoring a session or opening a socket. */
struct FLoomaComponentEnabledTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};
    UWorld* World = nullptr;

    FLoomaComponentEnabledTestAccess()
    {
        const UWorld::InitializationValues Values = UWorld::InitializationValues()
            .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
    }

    ~FLoomaComponentEnabledTestAccess()
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
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaComponentEnabledPatchTest,
    "Looma.Components.Enabled.Patch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaComponentEnabledPatchTest::RunTest(const FString& Parameters)
{
    FLoomaComponentEnabledTestAccess Access;
    ALoomaSyncedActor* Parent = Access.Seed(TEXT("parent"));
    ALoomaSyncedActor* Child = Access.Seed(TEXT("child"));
    Child->AttachToActor(Parent, FAttachmentTransformRules::KeepRelativeTransform);
    Child->SetActorRelativeLocation(FVector(10, 20, 30));

    const TCHAR* On = TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[
        {"type":"mesh"},{"type":"material","color":"#ff0000"},{"type":"light"}
    ]},{"id":"child","components":[{"type":"mesh"}]}]})");
    Access.Receive(On);
    TestNotNull(TEXT("mesh starts on"), Parent->MeshComponent.Get());
    TestNotNull(TEXT("light starts on"), Parent->LightComponent.Get());

    Access.Receive(TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[
        {"type":"mesh"},{"type":"material","color":"#ff0000","enabled":false},{"type":"light"}
    ]}]})"));
    if (TestNotNull(TEXT("switching off a material keeps its mesh"), Parent->MeshMaterial.Get()))
    {
        TestEqual(TEXT("the mesh uses the default colour"),
            Parent->MeshMaterial->K2_GetVectorParameterValue(TEXT("Color")), FLoomaMaterialSpec().Color);
    }
    Access.Receive(On);
    if (TestNotNull(TEXT("material comes back"), Parent->MeshMaterial.Get()))
    {
        TestEqual(TEXT("with its original colour"),
            Parent->MeshMaterial->K2_GetVectorParameterValue(TEXT("Color")), FLinearColor::Red);
    }

    // Stand-ins for already-loaded GLB/audio slots: disabling must tear down an
    // existing source as well as avoid starting a new download. No live backend.
    Parent->ModelComponent = NewObject<UStaticMeshComponent>(Parent);
    Parent->CharacterComponent = NewObject<USkeletalMeshComponent>(Parent);
    Parent->AudioComponent = NewObject<UAudioComponent>(Parent);

    Access.Receive(TEXT(R"({"type":"patch","nodes":[{"id":"parent","components":[
        {"type":"mesh","enabled":false},{"type":"material","color":"#ff0000","enabled":false},
        {"type":"light","enabled":false},{"type":"audio","assetId":"sound","enabled":false},
        {"type":"model","assetId":"chair","enabled":false}
    ]}]})"));
    TestNull(TEXT("mesh switches off"), Parent->MeshComponent.Get());
    TestNull(TEXT("light switches off"), Parent->LightComponent.Get());
    TestNull(TEXT("disabled sound starts no download or playback"), Parent->AudioComponent.Get());
    TestNull(TEXT("disabled model starts no download"), Parent->ModelComponent.Get());
    TestNull(TEXT("the character model also switches off"), Parent->CharacterComponent.Get());
    TestNotNull(TEXT("the child's mesh stays on"), Child->MeshComponent.Get());
    TestEqual(TEXT("the child keeps its local pose"), Child->GetLocalTransform().GetLocation(), FVector(10, 20, 30));
    TestEqual(TEXT("disabled definitions stay on the node"), Parent->WireComponents.Num(), 5);
    TestEqual(TEXT("the disabled model keeps its asset"),
        Parent->WireComponents.Last()->AsObject()->GetStringField(TEXT("assetId")), FString(TEXT("chair")));

    // Removing the key is the web editor's enable operation; no node respawn needed.
    Access.Receive(On);
    TestNotNull(TEXT("mesh comes back"), Parent->MeshComponent.Get());
    TestNotNull(TEXT("light comes back"), Parent->LightComponent.Get());
    TestEqual(TEXT("the same actor survives both patches"), Access.Sync->FindSyncedActor(TEXT("parent")), Parent);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
