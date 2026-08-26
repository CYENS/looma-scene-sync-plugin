#include "LoomaDownloadImageAction.h"

#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "LoomaSceneSyncSubsystem.h"

ULoomaDownloadImageAction* ULoomaDownloadImageAction::DownloadImageAsTexture(UObject* WorldContextObject, const FString& Url)
{
    ULoomaDownloadImageAction* Action = NewObject<ULoomaDownloadImageAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Url = Url;
    return Action;
}

void ULoomaDownloadImageAction::Activate()
{
    UGameInstance* GameInstance = WorldContextObject.IsValid()
        ? UGameplayStatics::GetGameInstance(WorldContextObject.Get())
        : nullptr;
    ULoomaSceneSyncSubsystem* Subsystem = GameInstance
        ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>()
        : nullptr;

    // Resolve backend-relative URLs; fall back to the raw URL if it's absolute.
    const FString ResolvedUrl = Subsystem ? Subsystem->ResolveBackendUrl(Url) : Url;
    if (ResolvedUrl.IsEmpty())
    {
        OnFailed.Broadcast(nullptr);
        SetReadyToDestroy();
        return;
    }

    if (GameInstance)
    {
        RegisterWithGameInstance(GameInstance);
    }

    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(ResolvedUrl);
    Request->SetVerb(TEXT("GET"));
    if (Subsystem)
    {
        // After SetURL, which matters more here than anywhere else: this URL came off a
        // generation job and through ResolveBackendUrl, which forwards an absolute URL
        // untouched — so it is the one request whose host we did not choose.
        // ApplyAuthHeader withholds the bearer unless the URL is the configured
        // backend, which is what stops a job naming another host from collecting this
        // session. Harmless when the image turns out to be under /static, which is not
        // gated: an unread header costs nothing.
        Subsystem->ApplyAuthHeader(Request);
    }
    Request->OnProcessRequestComplete().BindUObject(this, &ULoomaDownloadImageAction::OnResponse);
    Request->ProcessRequest();
}

void ULoomaDownloadImageAction::OnResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
    UTexture2D* Texture = nullptr;
    if (bConnectedSuccessfully && Response.IsValid() && Response->GetResponseCode() < 300)
    {
        const TArray<uint8>& Content = Response->GetContent();
        if (Content.Num() > 0)
        {
            // Handles PNG/JPEG; returns a transient UTexture2D (or null on failure).
            Texture = FImageUtils::ImportBufferAsTexture2D(Content);
        }
    }

    if (Texture)
    {
        OnLoaded.Broadcast(Texture);
    }
    else
    {
        OnFailed.Broadcast(nullptr);
    }
    SetReadyToDestroy();
}
