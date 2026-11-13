#pragma once

#include "CoreMinimal.h"

struct FLoomaNodeComponents;

/**
 * The scene's ground: which one a scene has, its heights, and the meshes they make
 * (looma-xr-asset-demo docs/scene-format.md, "The `terrain` component" and "The sea
 * floor"). Pure maths with no actor in it, so it is tested on its own;
 * ALoomaTerrainActor draws what this builds.
 *
 * THE GROUND IS ONE THING, as on the web (frontend/src/scene/terrain/format.js,
 * `activeGround`). Where an Ocean is switched on, its floor is the ground: tiles
 * over `floorSize` metres centred on the Ocean's host, heights relative to the water's
 * resting surface, every unshaped tile at the seabed's depth. Without one, the
 * scene's terrain is. A terrain under an Ocean is not drawn.
 *
 * THE HEIGHTS are a square grid of N × N, N = 64 per tile across, rows along wire +z
 * and the heights in a row along wire +x. Height (i, j) stands at wire
 *
 *   x = −size/2 + i · size/(N − 1),   z = −size/2 + j · size/(N − 1)
 *
 * from the host, which is where the web stands it (heightfield.js), so every client
 * stands the same ground in the same place.
 */
namespace LoomaTerrain
{
/** Heights a tile side (backend/app/terrain.py TILE). */
inline constexpr int32 Tile = 64;

/** One tile's bytes: Tile × Tile little-endian unsigned 16-bit heights. */
inline constexpr int32 TileBytes = Tile * Tile * 2;

/** The most tiles the backend hands out in one request, a whole 16 × 16 terrain. */
inline constexpr int32 MaxTilesPerRequest = 256;

/** A stored height: v is (v − 32768) / 256 wire metres above the host. */
inline float DecodeHeight(uint16 Value)
{
    return (static_cast<float>(Value) - 32768.0f) / 256.0f;
}

/** A height held as every client holds it: clamped to the encoding and rounded to its 1/256 m. */
LOOMASCENESYNC_API float Quantize(float Height);
}

/** The scene's ground, as LoomaChooseGround finds it. */
struct LOOMASCENESYNC_API FLoomaGround
{
    /** The node it is centred on: the Ocean's or the terrain's. Empty for no ground. */
    FString NodeId;

    /** An Ocean's floor: there is water, its resting surface at the host. */
    bool bOcean = false;

    /** Wire metres across the square. */
    float Size = 200.0f;

    /** Height of a null tile, wire metres above the host, quantized: a terrain's base, or minus the seabed. */
    float Base = 0.0f;

    /** Tiles a side. */
    int32 Across = 8;

    /** Across² tile ids, row by row along wire +z; an empty string is a null tile, flat at Base. */
    TArray<FString> Tiles;

    /** The same square on the same host, so only tiles may differ and only those need drawing again. */
    bool SameSquare(const FLoomaGround& Other) const
    {
        return NodeId == Other.NodeId && bOcean == Other.bOcean && Size == Other.Size && Base == Other.Base
            && Across == Other.Across;
    }
};

/** A node that may hold the ground. The caller leaves out nodes that are hidden or under a hidden one. */
struct FLoomaGroundCandidate
{
    FString Id;
    const FLoomaNodeComponents* Components = nullptr;
};

/**
 * The scene's ground among `Nodes`, into `Out`, or false for none. An Ocean that is
 * switched on wins over every terrain. Among several of a kind, the highest priority
 * wins, and a tie goes to the smallest node id, so every client picks the same one
 * whatever the order of the list (the web's `activeOcean` and `activeTerrain`).
 * Components switched off never reach here: the parser leaves them out.
 */
LOOMASCENESYNC_API bool LoomaChooseGround(const TArray<FLoomaGroundCandidate>& Nodes, FLoomaGround& Out);

/** A ground's heights in memory, in wire metres above its host. */
class LOOMASCENESYNC_API FLoomaHeightfield
{
public:
    /** Every height at `Base` (quantized), for a square `Size` metres across of `Across` tiles a side. */
    void Reset(int32 Across, float Size, float Base);

    /** Fill tile `T` from a stored tile's Tile² encoded heights, or with Base when `Values` is null. */
    void FillTile(int32 T, const uint16* Values);

    int32 GetAcross() const { return Across; }
    /** Heights a side: 64 per tile. */
    int32 GetN() const { return N; }
    float GetSize() const { return Size; }
    float GetBase() const { return Base; }
    /** Wire metres between neighbouring heights. */
    float GetSpacing() const { return N > 1 ? Size / static_cast<float>(N - 1) : 0.0f; }
    /** Wire x (or z) of height column (or row) `Index`, from the host. */
    float ToLocal(int32 Index) const { return -0.5f * Size + static_cast<float>(Index) * GetSpacing(); }

    /** Height (I, J), with I and J held to the square, as the web's texelFetch is. */
    float At(int32 I, int32 J) const
    {
        return Heights[FMath::Clamp(J, 0, N - 1) * N + FMath::Clamp(I, 0, N - 1)];
    }

    /** The lowest height anywhere. */
    float Lowest() const;

    /**
     * The ground's height at host-relative wire (X, Z), between the four heights round
     * it as the web's `terrainGround` interpolates, or false off the square.
     */
    bool HeightAt(float X, float Z, float& OutHeight) const;

private:
    int32 Across = 0;
    int32 N = 0;
    float Size = 0.0f;
    float Base = 0.0f;
    TArray<float> Heights;
};

/** One section of the ground's mesh, in Unreal centimetres and axes, relative to the host. */
struct FLoomaTerrainMesh
{
    TArray<FVector> Vertices;
    TArray<FVector> Normals;
    TArray<FVector2D> UV0;
    TArray<FColor> Colors;
    TArray<int32> Triangles;
};

/**
 * Tile `T`'s section of the ground: its heights and the first row and column of the
 * tiles after it, so neighbouring sections share their edge and the surface has no
 * seam. Each vertex is lit with the normal of the heights either side of it, as on
 * the web, so a change to a tile changes the normals of the tiles round it too: redraw
 * all eight. Triangles are wound as the web winds them, which faces up in Unreal as
 * well: the axis map turns the handedness over, and Unreal's front face is the other
 * way round from three.js's. With bTriangles false the index list is left empty, for
 * an update that only moves vertices.
 */
LOOMASCENESYNC_API void LoomaBuildTileMesh(const FLoomaHeightfield& Field, int32 T, bool bWater, bool bTriangles,
    FLoomaTerrainMesh& Out);

/**
 * The skirt round the square's edge, hanging from the edge heights down to `Bottom`
 * (wire metres above the host) and facing out, so the ground never shows its
 * thickness as a gap. The web hangs it to the seabed under an Ocean, and a metre below
 * the lowest ground without one.
 */
LOOMASCENESYNC_API void LoomaBuildSkirtMesh(const FLoomaHeightfield& Field, float Bottom, FLoomaTerrainMesh& Out);

/** The whole ground as one mesh of every `Stride`th height (and always the last), for collision. */
LOOMASCENESYNC_API void LoomaBuildCollisionMesh(const FLoomaHeightfield& Field, int32 Stride, FLoomaTerrainMesh& Out);

/**
 * The ground's colour, linear albedo, at host-relative wire (X, Z), Height metres above
 * the host, with wire normal `Normal`: the web's `terrainSurface` (terrain/shading.js)
 * worked out per vertex. Grass, and rock where the ground is steeper than about 40°;
 * with water (`bWater`), sand from the waterline up to a metre or so, wet and darker
 * at the waterline. The web varies it by a noise texture; this varies it by value noise
 * at the same three scales, so the look matches and the pattern does not.
 */
LOOMASCENESYNC_API FLinearColor LoomaGroundColor(float X, float Z, float Height, const FVector& Normal, bool bWater);
