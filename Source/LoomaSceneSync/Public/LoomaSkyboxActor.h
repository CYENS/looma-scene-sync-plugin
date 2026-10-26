#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LoomaSceneComponents.h"
#include "LoomaSkyboxActor.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;
class UTextureCube;
class USkyLightComponent;

/** One background per world, independent of the transform of its authoring node. */
UCLASS(NotBlueprintable)
class LOOMASCENESYNC_API ALoomaSkyboxActor : public AActor
{
    GENERATED_BODY()
public:
    ALoomaSkyboxActor();
    UFUNCTION(BlueprintCallable, Category = "Looma|Environment")
    void Apply(const FString& Url, const FLoomaSkyboxSpec& Spec);
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Dome;
    UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> Material;
    UPROPERTY() TObjectPtr<UTexture2D> Texture;
    UPROPERTY() TObjectPtr<UTextureCube> EnvironmentCube;
    UPROPERTY() TObjectPtr<USkyLightComponent> EnvironmentLight;
    TArray<TWeakObjectPtr<USkyLightComponent>> PreviousSkyLights;
    TArray<FLinearColor> SourcePixels;
    int32 SourceWidth = 0, SourceHeight = 0;
    FLoomaSkyboxSpec CurrentSpec;
    void UpdateLighting();
    void ReleaseLighting();
    FString CurrentUrl;
    int32 RequestVersion = 0;
};
