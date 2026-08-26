#include "LoomaSubmitGenerationAction.h"

#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "LoomaGenerationHandle.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaWireConvert.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

ULoomaSubmitGenerationAction* ULoomaSubmitGenerationAction::SubmitGeneration(UObject* WorldContextObject,
    const FString& Prompt, FTransform SuggestedTransform, bool bSuggestTransform, int32 NImages)
{
    ULoomaSubmitGenerationAction* Action = NewObject<ULoomaSubmitGenerationAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Prompt = Prompt;
    Action->SuggestedTransform = SuggestedTransform;
    Action->bSuggestTransform = bSuggestTransform;
    Action->NImages = NImages;
    return Action;
}

void ULoomaSubmitGenerationAction::Activate()
{
    UGameInstance* GameInstance = WorldContextObject.IsValid()
        ? UGameplayStatics::GetGameInstance(WorldContextObject.Get())
        : nullptr;
    ULoomaSceneSyncSubsystem* SyncSubsystem = GameInstance
        ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>()
        : nullptr;

    if (!SyncSubsystem)
    {
        OnFailed.Broadcast(TEXT("LoomaSceneSync subsystem unavailable"));
        SetReadyToDestroy();
        return;
    }
    Subsystem = SyncSubsystem;

    // Keep this action alive across the async HTTP round-trip.
    RegisterWithGameInstance(GameInstance);

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("prompt"), Prompt);
    Body->SetNumberField(TEXT("n_images"), FMath::Clamp(NImages, 1, 8));
    if (bSuggestTransform)
    {
        Body->SetObjectField(TEXT("transform"), LoomaUeToWire(SuggestedTransform));
    }

    FString BodyText;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&BodyText);
    FJsonSerializer::Serialize(Body, Writer);

    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(SyncSubsystem->GetRestBase() + TEXT("/generate"));
    Request->SetVerb(TEXT("POST"));
    // After SetURL: ApplyAuthHeader checks the URL is the configured backend before it
    // attaches anything. A generation job is attributed to whoever submitted it, so a
    // logged-in client must submit as itself and not as a guest.
    SyncSubsystem->ApplyAuthHeader(Request);
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(BodyText);
    Request->OnProcessRequestComplete().BindUObject(this, &ULoomaSubmitGenerationAction::OnResponse);
    Request->ProcessRequest();
}

void ULoomaSubmitGenerationAction::OnResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
    if (!bConnectedSuccessfully || !Response.IsValid() || Response->GetResponseCode() >= 300)
    {
        OnFailed.Broadcast(FString::Printf(TEXT("POST /generate failed (%d)"),
            Response.IsValid() ? Response->GetResponseCode() : 0));
        SetReadyToDestroy();
        return;
    }

    TSharedPtr<FJsonObject> Obj;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Response->GetContentAsString());
    FString JobId;
    if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
    {
        Obj->TryGetStringField(TEXT("job_id"), JobId);
    }

    if (JobId.IsEmpty())
    {
        OnFailed.Broadcast(TEXT("POST /generate returned no job_id"));
    }
    else
    {
        // Mint the handle before broadcasting so the Blueprint can bind its
        // events the moment it receives it. The job may already be cached (the
        // first WS event can beat this response) — the handle replays it next
        // tick, after binding.
        ULoomaGenerationHandle* Handle = Subsystem.IsValid()
            ? Subsystem->GetGenerationHandle(JobId)
            : nullptr;
        if (Subsystem.IsValid() && bSuggestTransform)
        {
            // We already know the pose we asked for. Registering it with the
            // subsystem (rather than just the handle) means the cache, the
            // hub-wide events and the handle all report it right away, instead of
            // waiting for an event the backend emits before storing the pose.
            Subsystem->NoteSuggestedTransform(JobId, SuggestedTransform);
        }
        OnSubmitted.Broadcast(JobId, Handle);
    }
    SetReadyToDestroy();
}
