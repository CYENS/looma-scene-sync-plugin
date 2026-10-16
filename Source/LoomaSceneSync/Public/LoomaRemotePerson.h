#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LoomaRemotePerson.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UTextRenderComponent;
struct FLoomaClientPose;

/**
 * Another client on this scene, drawn where it says it is (HAM-311 part 1): a head
 * in its roster colour with a dark visor on the side it looks out of, its two hands,
 * and its name above it. The same figure the web client draws
 * (frontend/src/scene/RemotePeople.jsx), so a person in a headset reads the same in
 * either viewer.
 *
 * Spawned, eased towards each new pose and destroyed by ULoomaSceneSyncSubsystem;
 * nothing here talks to the hub. No collision and no shadow: it is somebody standing
 * in the scene, not part of it, and nothing should trace against it, select it or
 * save it.
 */
UCLASS(NotPlaceable, Transient)
class LOOMASCENESYNC_API ALoomaRemotePerson : public AActor
{
    GENERATED_BODY()

public:
    ALoomaRemotePerson();

    /** Colour and name; cheap to call every frame, it only rebuilds what changed. */
    void SetLook(const FLinearColor& Color, const FString& Name);

    /**
     * Ease towards a pose: `Alpha` of the way there this frame, or all of it when
     * `bSnap` (the first pose, so a person does not fly in from the world origin).
     * `ViewLocation` turns the name to face whoever is looking.
     */
    void Follow(const FLoomaClientPose& Pose, float Alpha, bool bSnap, const FVector& ViewLocation);

    UPROPERTY(VisibleAnywhere, Category = "Looma|Presence")
    TObjectPtr<UStaticMeshComponent> Head;

    UPROPERTY(VisibleAnywhere, Category = "Looma|Presence")
    TObjectPtr<UStaticMeshComponent> Visor;

    UPROPERTY(VisibleAnywhere, Category = "Looma|Presence")
    TObjectPtr<UStaticMeshComponent> LeftHand;

    UPROPERTY(VisibleAnywhere, Category = "Looma|Presence")
    TObjectPtr<UStaticMeshComponent> RightHand;

    UPROPERTY(VisibleAnywhere, Category = "Looma|Presence")
    TObjectPtr<UTextRenderComponent> NameTag;

private:
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> BodyMaterial;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> VisorMaterial;

    FLinearColor ShownColor = FLinearColor::Transparent;
    FString ShownName;
    bool bHandsPlaced[2] = {false, false};
};
