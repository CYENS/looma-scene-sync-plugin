#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "LoomaDownloadImageAction.generated.h"

class UTexture2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaImageDownloaded, UTexture2D*, Texture);

/**
 * Async Blueprint node: download a candidate/selected image and decode it into a
 * transient UTexture2D (for the orb "portal"). The URL may be backend-relative
 * (e.g. "/api/static/..") — it is resolved against the backend host and the web
 * "/api" proxy prefix is stripped. OnFailed passes a null texture.
 */
UCLASS()
class LOOMASCENESYNC_API ULoomaDownloadImageAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FLoomaImageDownloaded OnLoaded;

    UPROPERTY(BlueprintAssignable)
    FLoomaImageDownloaded OnFailed;

    UFUNCTION(BlueprintCallable,
        meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
        Category = "Looma|Generation")
    static ULoomaDownloadImageAction* DownloadImageAsTexture(UObject* WorldContextObject, const FString& Url);

    virtual void Activate() override;

private:
    void OnResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);

    TWeakObjectPtr<UObject> WorldContextObject;
    FString Url;
};
