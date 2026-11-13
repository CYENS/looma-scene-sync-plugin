#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LoomaOceanWaves.h"
#include "LoomaOceanActor.generated.h"

class ALoomaTerrainActor;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UTexture2D;
struct FLoomaOceanSpec;

/** The active Ocean's water surface. Its waves follow the shared scene clock. */
UCLASS(NotBlueprintable)
class ALoomaOceanActor : public AActor
{
    GENERATED_BODY()

public:
    ALoomaOceanActor();

    void Show(AActor& Host, const FLoomaOceanSpec& Spec);
    void Clear();
    void UpdateSurface(ALoomaTerrainActor& Terrain, const FVector& ViewLocation, double HubSeconds);

private:
    void BuildSurface();
    void UpdateWaveTextures(double HubSeconds);
    void UpdateFloorTexture(const ALoomaTerrainActor& Terrain);

    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Mesh;
    UPROPERTY() TObjectPtr<UMaterialInterface> Material;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> Surface;
    UPROPERTY() TObjectPtr<UTexture2D> FloorTexture;

    UPROPERTY() TArray<TObjectPtr<UTexture2D>> WaveTextures;

    FLoomaOceanWaves Waves;
    TWeakObjectPtr<AActor> OceanHost;
    TWeakObjectPtr<ALoomaTerrainActor> FloorSource;
    uint64 FloorRevision = 0;
    float FloorSize = 0.0f;

    friend struct FLoomaTerrainTestAccess;
};
