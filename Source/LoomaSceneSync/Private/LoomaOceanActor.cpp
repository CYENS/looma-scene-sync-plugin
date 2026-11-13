#include "LoomaOceanActor.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "LoomaSceneComponents.h"
#include "LoomaTerrainActor.h"
#include "LoomaWireConvert.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
constexpr double CentimetersPerMeter = 100.0;
constexpr double RecenterStep = 400.0; // centimetres
const FName WindParameter(TEXT("Wind"));
const FName SeaParameter(TEXT("Sea"));
const FName FloorParameter(TEXT("Floor"));
const FName FloorHeightsParameter(TEXT("FloorHeights"));

struct FFloorUpload
{
    FUpdateTextureRegion2D Region;
    TArray<float> Heights;
};
} // namespace

ALoomaOceanActor::ALoomaOceanActor()
{
    PrimaryActorTick.bCanEverTick = false;
    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Water"));
    RootComponent = Mesh;
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetCanEverAffectNavigation(false);
    Mesh->SetCastShadow(false);
    Mesh->SetHiddenInGame(true);

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Water(
        TEXT("/LoomaSceneSync/Materials/M_LoomaOcean.M_LoomaOcean"));
    Material = Water.Object;
}

void ALoomaOceanActor::Show(AActor& Host, const FLoomaOceanSpec& Spec)
{
    if (!Material)
    {
        return;
    }
    if (!Surface)
    {
        Surface = UMaterialInstanceDynamic::Create(Material, this);
        BuildSurface();
        Mesh->SetMaterial(0, Surface);
    }
    OceanHost = &Host;

    // Wind direction is where the wave travels, in wire +X towards wire +Z.
    const double Radians = FMath::DegreesToRadians(Spec.WindDir);
    const FVector Direction = LoomaWirePointToUe(FVector(FMath::Cos(Radians), 0, FMath::Sin(Radians))).GetSafeNormal();
    const float WindHeight = 0.0885f * Spec.Wind;
    const float Shoal = FMath::Clamp(1.5f * FMath::Sqrt(FMath::Square(WindHeight) + FMath::Square(Spec.Swell)), 0.75f, 4.0f);
    Surface->SetVectorParameterValue(WindParameter, FLinearColor(Direction.X, Direction.Y, Spec.Wind, Spec.Swell));
    Surface->SetVectorParameterValue(SeaParameter, FLinearColor(Spec.Seabed, Shoal, 0.0f, 0.0f));
}

void ALoomaOceanActor::Clear()
{
    Mesh->SetHiddenInGame(true);
    OceanHost.Reset();
    FloorSource.Reset();
    // Retain the last allocation with this hidden surface so switching back to
    // an Ocean of the same resolution can reuse it. The next Show invalidates its
    // source and refreshes it before revealing the mesh.
}

void ALoomaOceanActor::UpdateSurface(ALoomaTerrainActor& Terrain, const FVector& ViewLocation)
{
    const AActor* Host = OceanHost.Get();
    if (!Host || !Surface || !Terrain.IsShowing() || !Terrain.IsSeaFloor() || Terrain.GetAttachParentActor() != Host)
    {
        Clear();
        return;
    }

    const FVector HostLocation = Host->GetActorLocation();
    // Keep the finest part under the viewer. The material samples absolute XY,
    // so moving the grid preserves the location of every crest.
    const FVector Center(FMath::GridSnap(ViewLocation.X, RecenterStep),
        FMath::GridSnap(ViewLocation.Y, RecenterStep), HostLocation.Z);
    if (GetActorLocation() != Center)
    {
        SetActorLocation(Center);
    }

    if (FloorSource.Get() != &Terrain || FloorRevision != Terrain.GetHeightfieldRevision())
    {
        UpdateFloorTexture(Terrain);
        FloorSource = &Terrain;
        FloorRevision = Terrain.GetHeightfieldRevision();
    }
    const FVector2D FloorOrigin = FVector2D(HostLocation) / CentimetersPerMeter;
    Surface->SetVectorParameterValue(FloorParameter, FLinearColor(FloorOrigin.X, FloorOrigin.Y,
        FloorSize > 0.0f ? 1.0f / FloorSize : 0.0f, FloorSize > 0.0f ? static_cast<float>(FloorTexture->GetSizeX()) : 0.0f));
    Mesh->SetHiddenInGame(false);
}

void ALoomaOceanActor::BuildSurface()
{
    // One connected grid, with metre steps near the viewer and progressively wider
    // cells out to 8 km. It keeps a continuous horizon without recreating sections.
    constexpr int32 NearSteps = 32;
    constexpr double Radius = 8000.0 * CentimetersPerMeter;
    TArray<double> Positive;
    for (int32 Step = 1; Step <= NearSteps; ++Step)
    {
        Positive.Add(Step * CentimetersPerMeter);
    }
    for (double Step = 120.0; Positive.Last() < Radius; Step *= 1.22)
    {
        Positive.Add(FMath::Min(Positive.Last() + Step, Radius));
    }
    TArray<double> Axis;
    Axis.Reserve(2 * Positive.Num() + 1);
    for (int32 I = Positive.Num() - 1; I >= 0; --I)
    {
        Axis.Add(-Positive[I]);
    }
    Axis.Add(0.0);
    Axis.Append(Positive);

    const int32 N = Axis.Num();
    TArray<FVector> Vertices;
    TArray<FVector> Normals;
    TArray<int32> Triangles;
    Vertices.Reserve(N * N);
    Normals.Init(FVector::UpVector, N * N);
    Triangles.Reserve(6 * (N - 1) * (N - 1));
    for (const double Y : Axis)
    {
        for (const double X : Axis)
        {
            Vertices.Emplace(X, Y, 0.0);
        }
    }
    for (int32 J = 0; J < N - 1; ++J)
    {
        for (int32 I = 0; I < N - 1; ++I)
        {
            const int32 A = J * N + I;
            const int32 B = A + 1;
            const int32 C = A + N;
            const int32 D = C + 1;
            Triangles.Append({A, C, B, B, C, D});
        }
    }
    Mesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, {}, {}, {}, false, false);
}

void ALoomaOceanActor::UpdateFloorTexture(const ALoomaTerrainActor& Terrain)
{
    const FLoomaHeightfield& Field = Terrain.GetHeightfield();
    const int32 N = Field.GetN();
    FloorSize = 0.0f;
    if (N < 2 || !Terrain.GetGround().Tiles.ContainsByPredicate([](const FString& Id) { return !Id.IsEmpty(); }))
    {
        return;
    }

    // The floor is already decoded by the terrain actor. The shader reads that
    // same array in wire X/Z order, including the old tile until its new id arrives.
    const TArrayView<const float> Heights = Field.GetHeights();
    if (!FloorTexture || FloorTexture->GetSizeX() != N)
    {
        FloorTexture = UTexture2D::CreateTransient(N, N, PF_R32_FLOAT);
        if (!FloorTexture)
        {
            return;
        }
        FloorTexture->SRGB = false;
        FloorTexture->Filter = TF_Bilinear;
        FloorTexture->AddressX = TA_Clamp;
        FloorTexture->AddressY = TA_Clamp;
        FloorTexture->NeverStream = true;
        FTexture2DMipMap& Mip = FloorTexture->GetPlatformData()->Mips[0];
        FMemory::Memcpy(Mip.BulkData.Lock(LOCK_READ_WRITE), Heights.GetData(), Heights.Num() * sizeof(float));
        Mip.BulkData.Unlock();
        FloorTexture->UpdateResource();
        Surface->SetTextureParameterValue(FloorHeightsParameter, FloorTexture);
    }
    else if (FloorTexture->GetResource())
    {
        // Keep the texture and its material binding through sculpt edits. The
        // render command owns this copy until the upload completes on the RHI.
        const TSharedRef<FFloorUpload, ESPMode::ThreadSafe> Upload = MakeShared<FFloorUpload, ESPMode::ThreadSafe>();
        Upload->Region = FUpdateTextureRegion2D(0, 0, 0, 0, N, N);
        Upload->Heights.Append(Heights.GetData(), Heights.Num());
        FloorTexture->UpdateTextureRegions(0, 1, &Upload->Region, N * sizeof(float), sizeof(float),
            reinterpret_cast<uint8*>(Upload->Heights.GetData()), [Upload](uint8*, const FUpdateTextureRegion2D*) {});
    }
    FloorSize = Field.GetSize();
}
