#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LoomaTerrain.h"
#include "LoomaTerrainActor.generated.h"

class UMaterialInterface;
class UProceduralMeshComponent;

/**
 * The scene's ground, drawn: a terrain, or an Ocean's floor (LoomaTerrain.h). One per
 * world, like the sky, spawned by the subsystem when a scene first has a ground.
 *
 * It is attached to the ground's host node with absolute rotation and scale, so it
 * stands on the host's world position, follows it when it moves, and lies level and
 * unscaled however the host is turned or scaled, as the format says a ground does.
 *
 * The heights arrive as tiles. A tile never changes under its id, so a sculpt stroke
 * on the web reaches here as a `patch` naming new ids for the tiles it touched: this
 * fetches only the ids it has not seen (POST /terrain/tiles/fetch), keeps every tile
 * it fetches, and redraws only the sections those tiles move. The mesh is one
 * UProceduralMeshComponent section per tile, plus the skirt. Collision is a second,
 * hidden mesh of the whole ground, cooked off the game thread once the ground has
 * stopped changing for a moment, because cooking is the slow part.
 */
UCLASS(NotBlueprintable)
class LOOMASCENESYNC_API ALoomaTerrainActor : public AActor
{
    GENERATED_BODY()

public:
    ALoomaTerrainActor();

    /**
     * Show `Ground` on `Host`, fetching whatever tiles it lacks from `RestBase`, the
     * backend's HTTP root. Cheap to call again: the same square with other tiles
     * redraws only those tiles and their neighbours, and a tile still on its way keeps
     * showing what was there before until it arrives.
     */
    void Show(const FLoomaGround& Ground, AActor* Host, const FString& RestBase);

    /** Show nothing, and let go of the host. Fetched tiles are kept. */
    void Clear();

    /**
     * Keep tiles: `Bytes` holds Ids.Num() of them back to back, as the backend sends
     * them. Whatever was waiting on them is drawn. The fetch's own completion, and the
     * way a test hands over tiles without a backend.
     */
    void AddTiles(const TArray<FString>& Ids, const TArray<uint8>& Bytes);

    /** Every frame, from the subsystem: a fetch retry when one is due, and the collision when it is. */
    void TickTerrain(float DeltaTime);

    /** Whether a ground is being shown. */
    UFUNCTION(BlueprintPure, Category = "Looma|Terrain")
    bool IsShowing() const { return !Ground.NodeId.IsEmpty(); }

    /** The id of the node the ground is centred on, empty for none. */
    UFUNCTION(BlueprintPure, Category = "Looma|Terrain")
    FString GetHostId() const { return Ground.NodeId; }

    /** Whether the ground is an Ocean's floor rather than a terrain. */
    UFUNCTION(BlueprintPure, Category = "Looma|Terrain")
    bool IsSeaFloor() const { return Ground.bOcean; }

    /**
     * The ground's height under a world point, as a world Z in centimetres, or false
     * when the point is not over the ground. Between heights it interpolates as the web
     * does, so a foot placed with it stands where the web would stand it.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Terrain")
    bool GetGroundHeight(const FVector& WorldLocation, float& OutWorldZ) const;

    const FLoomaGround& GetGround() const { return Ground; }
    const FLoomaHeightfield& GetHeightfield() const { return Field; }
    uint64 GetHeightfieldRevision() const { return HeightfieldRevision; }

    /** Tiles listed whose heights are not drawn yet, because they are still being fetched. */
    int32 GetWaitingTiles() const;

    /** Tiles kept, from every ground shown so far. */
    int32 GetKeptTiles() const { return Kept.Num(); }

    /** Tile ids being fetched now. */
    int32 GetTilesInFlight() const { return InFlight.Num(); }

    /** Tile ids the backend says it does not have. Drawn flat, at the ground's base. */
    int32 GetMissingTiles() const { return Missing.Num(); }

    /** Whether the collision is waiting for the ground to settle before it is cooked again. */
    bool IsCollisionPending() const { return CollisionIn >= 0.0f; }

    UProceduralMeshComponent* GetMesh() const { return Mesh; }
    UProceduralMeshComponent* GetCollisionMesh() const { return Collision; }

    /** Cook the collision now rather than when the ground settles. */
    void BuildCollision();

private:
    /** Draw the tiles whose ids changed, or every section with bAll; ask for the tiles not kept. */
    void Apply(bool bAll);

    void Fetch(const TArray<FString>& Ids);

    /** Rebuild the sections round `Changed` (or all of them) and the skirt, and schedule the collision. */
    void Redraw(const TArray<int32>& Changed, bool bAll);

    /** Let go of tiles nothing shows, once there are many. */
    void Prune();

    UPROPERTY() TObjectPtr<USceneComponent> Root;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Mesh;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Collision;
    UPROPERTY() TObjectPtr<UMaterialInterface> Material;

    FLoomaGround Ground;
    FString RestBase;
    FLoomaHeightfield Field;
    uint64 HeightfieldRevision = 0;

    /** Per tile, the id whose heights it holds now: empty for flat at the base, Unfilled for nothing yet. */
    TArray<FString> Shown;
    /** Per section (tiles, then the skirt), whether it has been created. */
    TArray<bool> Built;

    TMap<FString, TArray<uint16>> Kept;
    TSet<FString> InFlight;
    TSet<FString> Missing;

    /** Seconds until a failed fetch is tried again; negative when none is due. */
    float RetryIn = -1.0f;
    /** Seconds until the collision is cooked; negative when it is up to date. */
    float CollisionIn = -1.0f;
};
