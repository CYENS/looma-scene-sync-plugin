#include "LoomaSyncedActor.h"

#include "Components/StaticMeshComponent.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"

ALoomaSyncedActor::ALoomaSyncedActor()
{
    PrimaryActorTick.bCanEverTick = true;

    MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
    MeshComponent->SetMobility(EComponentMobility::Movable);
    RootComponent = MeshComponent;
}

void ALoomaSyncedActor::LoadMeshFromUrl(const FString& Url)
{
    // Default LoaderConfig: glTFRuntime's default SceneBasis/SceneScale is the
    // same Y-up-meters -> Z-up-centimeters conversion the sync layer uses for
    // transforms, so geometry and poses stay consistent. Do not override it.
    FglTFRuntimeConfig LoaderConfig;
    FglTFRuntimeHttpResponse Completed;
    Completed.BindDynamic(this, &ALoomaSyncedActor::OnGlbLoaded);
    UglTFRuntimeFunctionLibrary::glTFLoadAssetFromUrl(Url, TMap<FString, FString>(), Completed, LoaderConfig);
}

void ALoomaSyncedActor::OnGlbLoaded(UglTFRuntimeAsset* Asset)
{
    if (!IsValid(this) || !Asset)
    {
        UE_LOG(LogTemp, Warning, TEXT("LoomaSceneSync: failed to load GLB for asset '%s'"), *AssetId);
        return;
    }
    // Datalake GLBs are single objects (origin-centered, unit bounding box) —
    // merging the whole node tree into one static mesh is exactly right.
    FglTFRuntimeStaticMeshConfig MeshConfig;
    UStaticMesh* Mesh = Asset->LoadStaticMeshRecursive(TEXT(""), TArray<FString>(), MeshConfig);
    if (Mesh)
    {
        MeshComponent->SetStaticMesh(Mesh);
        // Datalake assets are normalized to a ~1 m (100 cm) bounding box; log
        // the built size so a scale/basis regression is visible in the log.
        const FBox Box = Mesh->GetBoundingBox();
        UE_LOG(LogTemp, Log, TEXT("LoomaSceneSync: mesh for '%s' built, bbox size %s (cm), center %s"),
            *AssetId, *Box.GetSize().ToString(), *Box.GetCenter().ToString());
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("LoomaSceneSync: GLB for '%s' produced no static mesh"), *AssetId);
    }
}

void ALoomaSyncedActor::SetRemoteTarget(const FTransform& Target, bool bSnap)
{
    if (bSnap)
    {
        bHasRemoteTarget = false;
        SetActorTransform(Target);
        return;
    }
    RemoteTarget = Target;
    bHasRemoteTarget = true;
}

void ALoomaSyncedActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bHasRemoteTarget)
    {
        return;
    }

    // Exponential ease toward the target — mirrors the web RemoteSmoother.
    const float Alpha = 1.0f - FMath::Exp(-12.0f * DeltaSeconds);
    const FTransform Current = GetActorTransform();
    FTransform Next;
    Next.SetLocation(FMath::Lerp(Current.GetLocation(), RemoteTarget.GetLocation(), Alpha));
    Next.SetRotation(FQuat::Slerp(Current.GetRotation(), RemoteTarget.GetRotation(), Alpha));
    Next.SetScale3D(FMath::Lerp(Current.GetScale3D(), RemoteTarget.GetScale3D(), Alpha));
    SetActorTransform(Next);

    const bool bClose =
        FVector::DistSquared(Next.GetLocation(), RemoteTarget.GetLocation()) < 0.01 && // (0.1 cm)^2
        Next.GetRotation().AngularDistance(RemoteTarget.GetRotation()) < 0.001 &&
        FVector::DistSquared(Next.GetScale3D(), RemoteTarget.GetScale3D()) < 1e-8;
    if (bClose)
    {
        SetActorTransform(RemoteTarget);
        bHasRemoteTarget = false;
    }
}
