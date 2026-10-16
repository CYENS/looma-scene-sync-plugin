#include "LoomaRemotePerson.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "LoomaPresenceTypes.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    // The engine's basic shapes are a metre across, so scale is size in metres.
    constexpr float HeadSize = 0.22f;
    constexpr float HandSize = 0.07f;
    constexpr float LabelHeight = 28.0f; // cm above the head's centre
    const FLinearColor VisorColor(0.006f, 0.007f, 0.009f);

    UStaticMeshComponent* MakePart(AActor* Owner, const TCHAR* Name, UStaticMesh* Mesh)
    {
        UStaticMeshComponent* Part = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
        Part->SetStaticMesh(Mesh);
        Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Part->SetGenerateOverlapEvents(false);
        Part->SetCastShadow(false);
        Part->SetCanEverAffectNavigation(false);
        return Part;
    }
}

ALoomaRemotePerson::ALoomaRemotePerson()
{
    PrimaryActorTick.bCanEverTick = false;
    SetActorEnableCollision(false);

    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));

    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);

    Head = MakePart(this, TEXT("Head"), Sphere.Object);
    Head->SetupAttachment(Root);
    Head->SetRelativeScale3D(FVector(HeadSize * 1.05f, HeadSize, HeadSize * 1.15f));

    // On the face, which is +X: a converted head looks where the headset looked.
    Visor = MakePart(this, TEXT("Visor"), Cube.Object);
    Visor->SetupAttachment(Head);
    Visor->SetRelativeLocation(FVector(42.0f, 0.0f, 5.0f)); // in the head's unscaled space
    Visor->SetRelativeScale3D(FVector(0.27f, 0.77f, 0.28f));

    LeftHand = MakePart(this, TEXT("LeftHand"), Sphere.Object);
    LeftHand->SetupAttachment(Root);
    LeftHand->SetRelativeScale3D(FVector(HandSize));
    RightHand = MakePart(this, TEXT("RightHand"), Sphere.Object);
    RightHand->SetupAttachment(Root);
    RightHand->SetRelativeScale3D(FVector(HandSize));

    NameTag = CreateDefaultSubobject<UTextRenderComponent>(TEXT("NameTag"));
    NameTag->SetupAttachment(Root);
    NameTag->SetHorizontalAlignment(EHTA_Center);
    NameTag->SetVerticalAlignment(EVRTA_TextCenter);
    NameTag->SetWorldSize(9.0f);
    NameTag->SetTextRenderColor(FColor(230, 233, 239));
    NameTag->SetCastShadow(false);
}

void ALoomaRemotePerson::SetLook(const FLinearColor& Color, const FString& Name)
{
    if (!BodyMaterial && Head->GetMaterial(0))
    {
        BodyMaterial = UMaterialInstanceDynamic::Create(Head->GetMaterial(0), this);
        Head->SetMaterial(0, BodyMaterial);
        LeftHand->SetMaterial(0, BodyMaterial);
        RightHand->SetMaterial(0, BodyMaterial);
        VisorMaterial = UMaterialInstanceDynamic::Create(Visor->GetMaterial(0), this);
        VisorMaterial->SetVectorParameterValue(TEXT("Color"), VisorColor);
        Visor->SetMaterial(0, VisorMaterial);
    }
    if (BodyMaterial && Color != ShownColor)
    {
        // BasicShapeMaterial's one parameter.
        BodyMaterial->SetVectorParameterValue(TEXT("Color"), Color);
        ShownColor = Color;
    }
    if (Name != ShownName)
    {
        NameTag->SetText(FText::FromString(Name));
        ShownName = Name;
    }
}

void ALoomaRemotePerson::Follow(const FLoomaClientPose& Pose, float Alpha, bool bSnap, const FVector& ViewLocation)
{
    auto Ease = [Alpha, bSnap](USceneComponent* Part, const FTransform& Target, bool bPlaced)
    {
        if (bSnap || !bPlaced)
        {
            Part->SetWorldLocationAndRotation(Target.GetLocation(), Target.GetRotation());
            return;
        }
        const FVector Location = FMath::Lerp(Part->GetComponentLocation(), Target.GetLocation(), Alpha);
        const FQuat Rotation = FQuat::Slerp(Part->GetComponentQuat(), Target.GetRotation(), Alpha);
        Part->SetWorldLocationAndRotation(Location, Rotation);
    };

    Ease(Head, Pose.Head, true);

    const bool bHas[2] = {Pose.bHasLeftHand, Pose.bHasRightHand};
    UStaticMeshComponent* Parts[2] = {LeftHand, RightHand};
    const FTransform* Targets[2] = {&Pose.LeftHand, &Pose.RightHand};
    for (int32 Side = 0; Side < 2; ++Side)
    {
        Parts[Side]->SetVisibility(bHas[Side]);
        if (bHas[Side])
        {
            Ease(Parts[Side], *Targets[Side], bHandsPlaced[Side]);
        }
        bHandsPlaced[Side] = bHas[Side];
    }

    // Above the head, turned to whoever is looking (a text render is one-sided).
    const FVector LabelAt = Head->GetComponentLocation() + FVector(0.0f, 0.0f, LabelHeight);
    const FVector ToViewer = ViewLocation - LabelAt;
    NameTag->SetWorldLocationAndRotation(LabelAt, FRotator(0.0f, ToViewer.Rotation().Yaw, 0.0f));
}
