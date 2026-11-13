#include "LoomaTerrainActor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/CollisionProfile.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaWireConvert.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ProceduralMeshComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
/** What a tile shows before anything has been put in it: never a tile id, never empty. */
const TCHAR* const TerrainUnfilled = TEXT("?");

/** Seconds the ground must stay still before its collision is cooked again. */
constexpr float TerrainCollisionDelay = 0.5f;

/** Seconds before a fetch that failed is tried again. */
constexpr float TerrainRetryDelay = 2.0f;

/**
 * Tiles kept before the ones nothing shows are let go. A sculpting session makes a new
 * tile for every tile a stroke touches a few times a second; each is 8 KB.
 */
constexpr int32 TerrainKeepAtMost = 1024;
} // namespace

ALoomaTerrainActor::ALoomaTerrainActor()
{
    PrimaryActorTick.bCanEverTick = false;

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent = Root;
    // Only the host's position counts: a ground lies level and unscaled.
    Root->SetUsingAbsoluteRotation(true);
    Root->SetUsingAbsoluteScale(true);

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Ground"));
    Mesh->SetupAttachment(Root);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetCastShadow(true);

    // The collision is its own mesh: a section update on a mesh with collision rewrites
    // the positions of every section's collision, and cooking it is the slow part, so
    // it waits until the ground stops changing.
    Collision = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GroundCollision"));
    Collision->SetupAttachment(Root);
    Collision->SetVisibility(false);
    Collision->SetHiddenInGame(true);
    Collision->bUseAsyncCooking = true;
    Collision->bUseComplexAsSimpleCollision = true;
    Collision->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Surface(
        TEXT("/LoomaSceneSync/Materials/M_LoomaTerrain.M_LoomaTerrain"));
    Material = Surface.Object;
}

void ALoomaTerrainActor::Show(const FLoomaGround& InGround, AActor* Host, const FString& InRestBase)
{
    const bool bNewSquare = !IsShowing() || !Ground.SameSquare(InGround);
    Ground = InGround;
    RestBase = InRestBase;
    const int32 Count = Ground.Across * Ground.Across;
    // The parser always makes the list whole; a ground built by hand might not.
    Ground.Tiles.SetNum(Count);

    if (Host && GetAttachParentActor() != Host)
    {
        AttachToActor(Host, FAttachmentTransformRules::KeepRelativeTransform);
    }
    Root->SetRelativeLocation(FVector::ZeroVector);
    Root->SetRelativeRotation(FQuat::Identity);
    Root->SetRelativeScale3D(FVector::OneVector);

    if (bNewSquare)
    {
        Field.Reset(Ground.Across, Ground.Size, Ground.Base);
        Shown.Init(TerrainUnfilled, Count);
        Built.Init(false, Count + 1);
        Mesh->ClearAllMeshSections();
    }
    Apply(bNewSquare);
}

void ALoomaTerrainActor::Clear()
{
    Ground = FLoomaGround();
    Shown.Reset();
    Built.Reset();
    Mesh->ClearAllMeshSections();
    Collision->ClearAllMeshSections();
    CollisionIn = -1.0f;
    RetryIn = -1.0f;
    if (GetAttachParentActor())
    {
        DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    }
}

void ALoomaTerrainActor::Apply(bool bAll)
{
    if (!IsShowing())
    {
        return;
    }
    TArray<int32> Changed;
    TArray<FString> Wanted;
    for (int32 T = 0; T < Ground.Tiles.Num(); ++T)
    {
        const FString& Id = Ground.Tiles[T];
        if (Shown[T] == Id)
        {
            continue;
        }
        if (Id.IsEmpty() || Missing.Contains(Id))
        {
            Field.FillTile(T, nullptr);
        }
        else if (const TArray<uint16>* Values = Kept.Find(Id))
        {
            Field.FillTile(T, Values->GetData());
        }
        else
        {
            // Still showing what it showed: the new heights replace it when they come.
            if (!InFlight.Contains(Id))
            {
                Wanted.AddUnique(Id);
            }
            continue;
        }
        Shown[T] = Id;
        Changed.Add(T);
    }
    if (bAll || Changed.Num() > 0)
    {
        Redraw(Changed, bAll);
    }
    if (Wanted.Num() > 0)
    {
        Fetch(Wanted);
    }
}

void ALoomaTerrainActor::Fetch(const TArray<FString>& Ids)
{
    if (RestBase.IsEmpty())
    {
        UE_LOG(LogLoomaSync, Warning, TEXT("Terrain: no backend address, so %d tiles cannot be fetched"), Ids.Num());
        return;
    }
    using LoomaTerrain::MaxTilesPerRequest;
    for (int32 Start = 0; Start < Ids.Num(); Start += MaxTilesPerRequest)
    {
        const TArray<FString> Batch(Ids.GetData() + Start, FMath::Min(MaxTilesPerRequest, Ids.Num() - Start));
        InFlight.Append(Batch);

        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& Id : Batch)
        {
            Values.Add(MakeShared<FJsonValueString>(Id));
        }
        const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
        Body->SetArrayField(TEXT("ids"), Values);
        FString Text;
        FJsonSerializer::Serialize(Body, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));

        const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(RestBase + TEXT("/terrain/tiles/fetch"));
        Request->SetVerb(TEXT("POST"));
        Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
        Request->SetContentAsString(Text);
        const TWeakObjectPtr<ALoomaTerrainActor> Weak(this);
        Request->OnProcessRequestComplete().BindLambda(
            [Weak, Batch](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
            {
                ALoomaTerrainActor* Self = Weak.Get();
                if (!Self)
                {
                    return;
                }
                for (const FString& Id : Batch)
                {
                    Self->InFlight.Remove(Id);
                }
                const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
                if (bConnected && Code == 200 && Response->GetContent().Num() == Batch.Num() * LoomaTerrain::TileBytes)
                {
                    Self->AddTiles(Batch, Response->GetContent());
                    return;
                }
                if (bConnected && Code == 404)
                {
                    // {"detail": {"reason": "no_tile", "missing": [...]}}: those are drawn
                    // flat, and the rest of the batch is asked for again without them.
                    TSharedPtr<FJsonObject> Json;
                    const TSharedPtr<FJsonObject>* Detail = nullptr;
                    const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
                    int32 Found = 0;
                    if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json)
                        && Json.IsValid() && Json->TryGetObjectField(TEXT("detail"), Detail) && Detail
                        && (*Detail)->TryGetArrayField(TEXT("missing"), Listed) && Listed)
                    {
                        for (const TSharedPtr<FJsonValue>& Value : *Listed)
                        {
                            FString Id;
                            if (Value.IsValid() && Value->TryGetString(Id))
                            {
                                Self->Missing.Add(Id);
                                ++Found;
                            }
                        }
                    }
                    if (Found > 0)
                    {
                        UE_LOG(LogLoomaSync, Warning, TEXT("Terrain: the backend has no tile for %d ids; drawing them flat"),
                            Found);
                        Self->Apply(false);
                        return;
                    }
                }
                UE_LOG(LogLoomaSync, Warning, TEXT("Terrain: fetching %d tiles failed (HTTP %d); trying again in %.0f s"),
                    Batch.Num(), Code, TerrainRetryDelay);
                Self->RetryIn = TerrainRetryDelay;
            });
        Request->ProcessRequest();
    }
}

void ALoomaTerrainActor::AddTiles(const TArray<FString>& Ids, const TArray<uint8>& Bytes)
{
    using LoomaTerrain::Tile;
    using LoomaTerrain::TileBytes;
    if (Bytes.Num() != Ids.Num() * TileBytes)
    {
        UE_LOG(LogLoomaSync, Warning, TEXT("Terrain: %d bytes is not %d tiles"), Bytes.Num(), Ids.Num());
        return;
    }
    for (int32 K = 0; K < Ids.Num(); ++K)
    {
        TArray<uint16>& Values = Kept.FindOrAdd(Ids[K]);
        Values.SetNumUninitialized(Tile * Tile);
        const uint8* Source = Bytes.GetData() + K * TileBytes;
        for (int32 V = 0; V < Tile * Tile; ++V)
        {
            // Little-endian, whatever this machine is.
            Values[V] = static_cast<uint16>(Source[2 * V] | (Source[2 * V + 1] << 8));
        }
    }
    Apply(false);
    Prune();
}

void ALoomaTerrainActor::Redraw(const TArray<int32>& Changed, bool bAll)
{
    const int32 Across = Ground.Across;
    const int32 Count = Across * Across;
    // A tile's heights also light its neighbours' edges, and the sections before it
    // carry its first row and column: redraw the ring round each changed tile.
    TArray<bool> Dirty;
    Dirty.Init(bAll, Count);
    for (const int32 T : Changed)
    {
        const int32 TX = T % Across;
        const int32 TZ = T / Across;
        for (int32 DZ = -1; DZ <= 1; ++DZ)
        {
            for (int32 DX = -1; DX <= 1; ++DX)
            {
                const int32 X = TX + DX;
                const int32 Z = TZ + DZ;
                if (X >= 0 && X < Across && Z >= 0 && Z < Across)
                {
                    Dirty[Z * Across + X] = true;
                }
            }
        }
    }

    UMaterialInterface* Surface = Material ? Material.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
    const TArray<FProcMeshTangent> NoTangents;
    FLoomaTerrainMesh Section;
    for (int32 T = 0; T < Count; ++T)
    {
        if (!Dirty[T])
        {
            continue;
        }
        if (!Built[T])
        {
            LoomaBuildTileMesh(Field, T, Ground.bOcean, true, Section);
            Mesh->CreateMeshSection(T, Section.Vertices, Section.Triangles, Section.Normals, Section.UV0,
                Section.Colors, NoTangents, false);
            Mesh->SetMaterial(T, Surface);
            Built[T] = true;
        }
        else
        {
            // Moves the vertices in place, without rebuilding the whole mesh's render state.
            LoomaBuildTileMesh(Field, T, Ground.bOcean, false, Section);
            Mesh->UpdateMeshSection(T, Section.Vertices, Section.Normals, Section.UV0, Section.Colors, NoTangents);
        }
    }

    // The skirt: to the seabed under an Ocean, a metre below the lowest ground without one.
    const float Bottom = Ground.bOcean ? Field.GetBase() : Field.Lowest() - 1.0f;
    LoomaBuildSkirtMesh(Field, Bottom, Section);
    if (!Built[Count])
    {
        Mesh->CreateMeshSection(Count, Section.Vertices, Section.Triangles, Section.Normals, Section.UV0,
            Section.Colors, NoTangents, false);
        Mesh->SetMaterial(Count, Surface);
        Built[Count] = true;
    }
    else
    {
        Mesh->UpdateMeshSection(Count, Section.Vertices, Section.Normals, Section.UV0, Section.Colors, NoTangents);
    }

    CollisionIn = TerrainCollisionDelay;
}

void ALoomaTerrainActor::BuildCollision()
{
    CollisionIn = -1.0f;
    if (!IsShowing())
    {
        Collision->ClearAllMeshSections();
        return;
    }
    // Every height up to 512 a side, every other one past that: a 16 × 16 terrain is a
    // million heights, and the collision does not need them all to stand a foot.
    const int32 Stride = Field.GetN() > 512 ? 2 : 1;
    FLoomaTerrainMesh Shape;
    LoomaBuildCollisionMesh(Field, Stride, Shape);
    Collision->CreateMeshSection(0, Shape.Vertices, Shape.Triangles, TArray<FVector>(), TArray<FVector2D>(),
        TArray<FColor>(), TArray<FProcMeshTangent>(), true);
}

void ALoomaTerrainActor::TickTerrain(float DeltaTime)
{
    if (RetryIn >= 0.0f)
    {
        RetryIn -= DeltaTime;
        if (RetryIn < 0.0f)
        {
            Apply(false);
        }
    }
    if (CollisionIn >= 0.0f)
    {
        CollisionIn -= DeltaTime;
        if (CollisionIn < 0.0f)
        {
            BuildCollision();
        }
    }
}

bool ALoomaTerrainActor::GetGroundHeight(const FVector& WorldLocation, float& OutWorldZ) const
{
    OutWorldZ = 0.0f;
    if (!IsShowing())
    {
        return false;
    }
    const FVector Origin = Root->GetComponentLocation();
    const FVector Wire = LoomaUePointToWire(WorldLocation - Origin);
    float Height = 0.0f;
    if (!Field.HeightAt(static_cast<float>(Wire.X), static_cast<float>(Wire.Z), Height))
    {
        return false;
    }
    OutWorldZ = static_cast<float>(Origin.Z) + Height * 100.0f;
    return true;
}

int32 ALoomaTerrainActor::GetWaitingTiles() const
{
    int32 Waiting = 0;
    for (int32 T = 0; T < Ground.Tiles.Num() && T < Shown.Num(); ++T)
    {
        Waiting += Shown[T] != Ground.Tiles[T] ? 1 : 0;
    }
    return Waiting;
}

void ALoomaTerrainActor::Prune()
{
    if (Kept.Num() <= TerrainKeepAtMost)
    {
        return;
    }
    const TSet<FString> Needed(Ground.Tiles);
    for (auto It = Kept.CreateIterator(); It; ++It)
    {
        if (!Needed.Contains(It.Key()))
        {
            It.RemoveCurrent();
        }
    }
}
