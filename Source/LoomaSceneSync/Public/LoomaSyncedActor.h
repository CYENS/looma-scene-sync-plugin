#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LoomaSyncedActor.generated.h"

class UglTFRuntimeAsset;
class UStaticMeshComponent;

/**
 * One synced scene object. The mesh is fetched at runtime from the backend
 * (GET /static/<assetId>.glb) via glTFRuntime; the actor's transform mirrors
 * the shared scene state. Remote transient transforms ease toward a target in
 * Tick (same k=12/s constant as the web app's RemoteSmoother); final
 * transforms snap exactly.
 */
UCLASS()
class LOOMASCENESYNC_API ALoomaSyncedActor : public AActor
{
    GENERATED_BODY()

public:
    ALoomaSyncedActor();

    /** Instance identity on the sync wire (UUIDv4). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString Guid;

    /** Datalake asset id (resolves to <backend>/static/<AssetId>.glb). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString AssetId;

    /** Display name from the catalog. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString DisplayName;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TObjectPtr<UStaticMeshComponent> MeshComponent;

    /** Kick off the async GLB download + mesh build. */
    void LoadMeshFromUrl(const FString& Url);

    /** Set where a remote peer wants this actor. bSnap = final (exact) pose. */
    void SetRemoteTarget(const FTransform& Target, bool bSnap);

    /** True while easing toward a remote target (outbound diff skips these). */
    bool HasRemoteTarget() const { return bHasRemoteTarget; }

    virtual void Tick(float DeltaSeconds) override;

private:
    UFUNCTION()
    void OnGlbLoaded(UglTFRuntimeAsset* Asset);

    FTransform RemoteTarget;
    bool bHasRemoteTarget = false;
};
