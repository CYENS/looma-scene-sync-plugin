#include "LoomaTerrain.h"

#include "LoomaSceneComponents.h"
#include "LoomaWireConvert.h"


float LoomaTerrain::Quantize(float Height)
{
    // format.js HEIGHT_LIMITS: what one uint16 of the encoding spans.
    const float Clamped = FMath::Clamp(Height, -128.0f, 32767.0f / 256.0f);
    return FMath::RoundHalfFromZero(Clamped * 256.0f) / 256.0f;
}

bool LoomaChooseGround(const TArray<FLoomaGroundCandidate>& Nodes, FLoomaGround& Out)
{
    // Highest priority, then the smallest id: oceanSettings.js `activeOcean` and
    // format.js `activeTerrain`. Ids compare as the web's `<` compares strings, code
    // unit by code unit, which is what a case-sensitive FString compare does.
    auto Pick = [&Nodes](auto Has, auto PriorityOf) -> const FLoomaGroundCandidate*
    {
        const FLoomaGroundCandidate* Chosen = nullptr;
        float Best = 0.0f;
        for (const FLoomaGroundCandidate& Node : Nodes)
        {
            if (!Node.Components || !Has(*Node.Components))
            {
                continue;
            }
            const float Priority = PriorityOf(*Node.Components);
            if (!Chosen || Priority > Best
                || (Priority == Best && Node.Id.Compare(Chosen->Id, ESearchCase::CaseSensitive) < 0))
            {
                Chosen = &Node;
                Best = Priority;
            }
        }
        return Chosen;
    };

    Out = FLoomaGround();
    const FLoomaGroundCandidate* Ocean = Pick(
        [](const FLoomaNodeComponents& C) { return C.bHasOcean; },
        [](const FLoomaNodeComponents& C) { return C.Ocean.Priority; });
    if (Ocean)
    {
        const FLoomaOceanSpec& Spec = Ocean->Components->Ocean;
        Out.NodeId = Ocean->Id;
        Out.bOcean = true;
        Out.Size = Spec.FloorSize;
        Out.Base = LoomaTerrain::Quantize(-Spec.Seabed);
        Out.Across = Spec.Across;
        Out.Tiles = Spec.Floor;
        return true;
    }
    const FLoomaGroundCandidate* Terrain = Pick(
        [](const FLoomaNodeComponents& C) { return C.bHasTerrain; },
        [](const FLoomaNodeComponents& C) { return C.Terrain.Priority; });
    if (Terrain)
    {
        const FLoomaTerrainSpec& Spec = Terrain->Components->Terrain;
        Out.NodeId = Terrain->Id;
        Out.Size = Spec.Size;
        Out.Base = LoomaTerrain::Quantize(Spec.Base);
        Out.Across = Spec.Across;
        Out.Tiles = Spec.Tiles;
        return true;
    }
    return false;
}

// --- Heights --------------------------------------------------------------------

void FLoomaHeightfield::Reset(int32 InAcross, float InSize, float InBase)
{
    Across = FMath::Max(InAcross, 1);
    N = Across * LoomaTerrain::Tile;
    Size = InSize;
    Base = LoomaTerrain::Quantize(InBase);
    Heights.Init(Base, N * N);
}

void FLoomaHeightfield::FillTile(int32 T, const uint16* Values)
{
    if (T < 0 || T >= Across * Across)
    {
        return;
    }
    const int32 X0 = (T % Across) * LoomaTerrain::Tile;
    const int32 Z0 = (T / Across) * LoomaTerrain::Tile;
    for (int32 J = 0; J < LoomaTerrain::Tile; ++J)
    {
        float* Row = Heights.GetData() + (Z0 + J) * N + X0;
        if (Values)
        {
            for (int32 I = 0; I < LoomaTerrain::Tile; ++I)
            {
                Row[I] = LoomaTerrain::DecodeHeight(Values[J * LoomaTerrain::Tile + I]);
            }
        }
        else
        {
            for (int32 I = 0; I < LoomaTerrain::Tile; ++I)
            {
                Row[I] = Base;
            }
        }
    }
}

float FLoomaHeightfield::Lowest() const
{
    float Low = Heights.Num() ? Heights[0] : Base;
    for (const float Height : Heights)
    {
        Low = FMath::Min(Low, Height);
    }
    return Low;
}

bool FLoomaHeightfield::HeightAt(float X, float Z, float& OutHeight) const
{
    OutHeight = 0.0f;
    if (N < 2)
    {
        return false;
    }
    const float Half = 0.5f * Size;
    if (X < -Half || X > Half || Z < -Half || Z > Half)
    {
        return false;
    }
    const float FX = (X + Half) / GetSpacing();
    const float FZ = (Z + Half) / GetSpacing();
    const int32 CX = FMath::Min(FMath::FloorToInt32(FX), N - 2);
    const int32 CZ = FMath::Min(FMath::FloorToInt32(FZ), N - 2);
    const float TX = FX - static_cast<float>(CX);
    const float TZ = FZ - static_cast<float>(CZ);
    const float Near = FMath::Lerp(At(CX, CZ), At(CX + 1, CZ), TX);
    const float Far = FMath::Lerp(At(CX, CZ + 1), At(CX + 1, CZ + 1), TX);
    OutHeight = FMath::Lerp(Near, Far, TZ);
    return true;
}

// --- Colour -----------------------------------------------------------------------

namespace LoomaTerrainDetail
{
uint32 Hash(int32 X, int32 Z, uint32 Channel)
{
    uint32 H = static_cast<uint32>(X) * 0x8da6b343u ^ static_cast<uint32>(Z) * 0xd8163841u ^ Channel * 0xcb1ab31fu;
    H ^= H >> 13;
    H *= 0x5bd1e995u;
    H ^= H >> 15;
    return H;
}

float Lattice(int32 X, int32 Z, uint32 Channel)
{
    return static_cast<float>(Hash(X, Z, Channel) & 0xffffffu) / static_cast<float>(0xffffffu);
}

/** Smooth value noise in 0…1, one lattice cell per unit. */
float Noise(float X, float Z, uint32 Channel)
{
    const float FX = FMath::FloorToFloat(X);
    const float FZ = FMath::FloorToFloat(Z);
    const int32 IX = static_cast<int32>(FX);
    const int32 IZ = static_cast<int32>(FZ);
    float TX = X - FX;
    float TZ = Z - FZ;
    TX = TX * TX * (3.0f - 2.0f * TX);
    TZ = TZ * TZ * (3.0f - 2.0f * TZ);
    const float Near = FMath::Lerp(Lattice(IX, IZ, Channel), Lattice(IX + 1, IZ, Channel), TX);
    const float Far = FMath::Lerp(Lattice(IX, IZ + 1, Channel), Lattice(IX + 1, IZ + 1, Channel), TX);
    return FMath::Lerp(Near, Far, TZ);
}

/** The four channels of the noise at one scale: the web's rgba of its noise tile. */
FVector4f Noise4(float X, float Z, float Scale, float Shift)
{
    const float U = X / Scale + Shift;
    const float V = Z / Scale + Shift;
    return FVector4f(Noise(U, V, 0), Noise(U, V, 1), Noise(U, V, 2), Noise(U, V, 3));
}

float SmoothStep(float Low, float High, float Value)
{
    const float T = FMath::Clamp((Value - Low) / (High - Low), 0.0f, 1.0f);
    return T * T * (3.0f - 2.0f * T);
}

// terrain/shading.js TERRAIN_SURFACE_GLSL, linear albedo.
const FLinearColor Sand(0.40f, 0.32f, 0.21f);
const FLinearColor SandDark(0.28f, 0.22f, 0.14f);
const FLinearColor Grass(0.075f, 0.13f, 0.035f);
const FLinearColor GrassDry(0.17f, 0.19f, 0.07f);
const FLinearColor Rock(0.20f, 0.19f, 0.17f);
const FLinearColor RockLight(0.34f, 0.32f, 0.29f);
const FLinearColor Earth(0.16f, 0.12f, 0.08f);

float Grain(float X, float Z)
{
    return Noise4(X, Z, 0.9f, 0.11f).Y;
}

/**
 * Wire direction to Unreal: the axis map of LoomaWireConvert without the metres to
 * centimetres. The map is a rotation and a reflection, so it takes a normal as it
 * takes any other direction.
 */
FVector WireToUeDirection(const FVector& V)
{
    return FVector(-V.Z, V.X, V.Y);
}

/** The ground's wire normal at height (I, J), from the heights either side, as the web's vertex shader. */
FVector WireNormalAt(const FLoomaHeightfield& Field, int32 I, int32 J)
{
    const FVector Normal(Field.At(I - 1, J) - Field.At(I + 1, J), 2.0f * Field.GetSpacing(),
        Field.At(I, J - 1) - Field.At(I, J + 1));
    return Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector(0, 1, 0));
}

void AddGroundVertex(const FLoomaHeightfield& Field, int32 I, int32 J, bool bWater, FLoomaTerrainMesh& Out)
{
    const float X = Field.ToLocal(I);
    const float Z = Field.ToLocal(J);
    const float H = Field.At(I, J);
    const FVector Normal = WireNormalAt(Field, I, J);
    Out.Vertices.Add(LoomaWirePointToUe(FVector(X, H, Z)));
    Out.Normals.Add(WireToUeDirection(Normal));
    Out.UV0.Add(FVector2D(X, Z));
    // sRGB-encoded so the darks keep their steps in eight bits; M_LoomaTerrain decodes it.
    Out.Colors.Add(LoomaGroundColor(X, Z, H, Normal, bWater).ToFColor(true));
}

/**
 * Two triangles a cell of a Cols × Rows grid of vertices, as the web's
 * `createTerrainGeometry` makes them: a, b, c and c, b, d, where b is the next row.
 */
void AddGrid(int32 Cols, int32 Rows, TArray<int32>& Triangles)
{
    Triangles.Reserve(Triangles.Num() + (Cols - 1) * (Rows - 1) * 6);
    for (int32 J = 0; J + 1 < Rows; ++J)
    {
        for (int32 I = 0; I + 1 < Cols; ++I)
        {
            const int32 A = J * Cols + I;
            const int32 B = A + Cols;
            const int32 C = A + 1;
            const int32 D = B + 1;
            Triangles.Append({A, B, C, C, B, D});
        }
    }
}

/**
 * Triangle A, B, C wound so its front faces `Facing`. Unreal's front face is the one
 * whose normal is (P1 − P2) × (P0 − P2) (KismetProceduralMeshLibrary's tangents, and
 * the engine's own box mesh).
 */
void AddFacing(FLoomaTerrainMesh& Out, int32 A, int32 B, int32 C, const FVector& Facing)
{
    const FVector Normal = (Out.Vertices[B] - Out.Vertices[C]) ^ (Out.Vertices[A] - Out.Vertices[C]);
    if ((Normal | Facing) < 0.0)
    {
        Swap(B, C);
    }
    Out.Triangles.Append({A, B, C});
}

void ResetMesh(FLoomaTerrainMesh& Out)
{
    Out.Vertices.Reset();
    Out.Normals.Reset();
    Out.UV0.Reset();
    Out.Colors.Reset();
    Out.Triangles.Reset();
}
} // namespace LoomaTerrainDetail

FLinearColor LoomaGroundColor(float X, float Z, float Height, const FVector& Normal, bool bWater)
{
    using namespace LoomaTerrainDetail;
    const FVector4f Big = Noise4(X, Z, 41.0f, 0.0f);
    const FVector4f Mid = Noise4(X, Z, 7.3f, 0.37f);
    const float GrainValue = Grain(X, Z);
    const float Water = bWater ? 1.0f : 0.0f;
    // Heights are relative to the host, and an Ocean's host is its resting surface.
    const float Above = Height;
    const float Slope = 1.0f - static_cast<float>(Normal.Y);

    const FLinearColor SandColor = FMath::Lerp(SandDark, Sand, SmoothStep(0.3f, 0.7f, Big.X * 0.6f + Mid.X * 0.4f));
    const FLinearColor GrassColor = FMath::Lerp(Grass, GrassDry, SmoothStep(0.35f, 0.75f, Big.Z * 0.7f + Mid.Y * 0.3f));
    const FLinearColor RockColor = FMath::Lerp(Rock, RockLight, SmoothStep(0.3f, 0.8f, Mid.W * 0.6f + GrainValue * 0.4f));
    // Sand from the water up to a metre or so, its edge wandering.
    const float Beach = 0.6f + 0.9f * Big.W;
    const float SandWeight = Water * (1.0f - SmoothStep(Beach - 0.35f, Beach + 0.35f, Above + (Mid.Z - 0.5f) * 0.6f));
    FLinearColor Color = FMath::Lerp(GrassColor, SandColor, SandWeight);
    const float RockWeight = SmoothStep(0.22f, 0.34f, Slope + (Mid.X - 0.5f) * 0.12f);
    Color = FMath::Lerp(Color, RockColor, RockWeight);
    // Wet just above the waterline, and under it.
    const float Wet = Water * (1.0f - SmoothStep(0.0f, 0.35f, Above));
    Color *= FMath::Lerp(1.0f, 0.62f, Wet * SandWeight);
    Color *= 0.85f + 0.3f * GrainValue;
    Color.A = 1.0f;
    return Color;
}

// --- Meshes -------------------------------------------------------------------------

void LoomaBuildTileMesh(const FLoomaHeightfield& Field, int32 T, bool bWater, bool bTriangles, FLoomaTerrainMesh& Out)
{
    using namespace LoomaTerrainDetail;
    ResetMesh(Out);
    const int32 Across = Field.GetAcross();
    const int32 N = Field.GetN();
    if (Across <= 0 || T < 0 || T >= Across * Across)
    {
        return;
    }
    const int32 I0 = (T % Across) * LoomaTerrain::Tile;
    const int32 J0 = (T / Across) * LoomaTerrain::Tile;
    const int32 I1 = FMath::Min(I0 + LoomaTerrain::Tile, N - 1);
    const int32 J1 = FMath::Min(J0 + LoomaTerrain::Tile, N - 1);
    const int32 Cols = I1 - I0 + 1;
    const int32 Rows = J1 - J0 + 1;
    Out.Vertices.Reserve(Cols * Rows);
    Out.Normals.Reserve(Cols * Rows);
    Out.UV0.Reserve(Cols * Rows);
    Out.Colors.Reserve(Cols * Rows);
    for (int32 J = J0; J <= J1; ++J)
    {
        for (int32 I = I0; I <= I1; ++I)
        {
            AddGroundVertex(Field, I, J, bWater, Out);
        }
    }
    if (bTriangles)
    {
        AddGrid(Cols, Rows, Out.Triangles);
    }
}

void LoomaBuildSkirtMesh(const FLoomaHeightfield& Field, float Bottom, FLoomaTerrainMesh& Out)
{
    using namespace LoomaTerrainDetail;
    ResetMesh(Out);
    const int32 N = Field.GetN();
    if (N < 2)
    {
        return;
    }
    struct FEdge
    {
        bool bAlongX;
        int32 Fixed;
        FVector Outward; // wire
    };
    const FEdge Edges[] = {
        {true, 0, FVector(0, 0, -1)},
        {true, N - 1, FVector(0, 0, 1)},
        {false, 0, FVector(-1, 0, 0)},
        {false, N - 1, FVector(1, 0, 0)},
    };
    for (const FEdge& Edge : Edges)
    {
        const int32 First = Out.Vertices.Num();
        const FVector Facing = WireToUeDirection(Edge.Outward);
        for (int32 K = 0; K < N; ++K)
        {
            const int32 I = Edge.bAlongX ? K : Edge.Fixed;
            const int32 J = Edge.bAlongX ? Edge.Fixed : K;
            const float X = Field.ToLocal(I);
            const float Z = Field.ToLocal(J);
            const float H = Field.At(I, J);
            const FColor Color = (Earth * (0.8f + 0.4f * Grain(X, Z))).ToFColor(true);
            const float Along = Edge.bAlongX ? X : Z;
            for (const float Y : {H, Bottom})
            {
                Out.Vertices.Add(LoomaWirePointToUe(FVector(X, Y, Z)));
                Out.Normals.Add(Facing);
                Out.UV0.Add(FVector2D(Along, Y));
                Out.Colors.Add(Color);
            }
        }
        for (int32 K = 0; K + 1 < N; ++K)
        {
            const int32 Top0 = First + 2 * K;
            const int32 Low0 = Top0 + 1;
            const int32 Top1 = Top0 + 2;
            const int32 Low1 = Top0 + 3;
            AddFacing(Out, Top0, Top1, Low0, Facing);
            AddFacing(Out, Low0, Top1, Low1, Facing);
        }
    }
}

void LoomaBuildCollisionMesh(const FLoomaHeightfield& Field, int32 Stride, FLoomaTerrainMesh& Out)
{
    using namespace LoomaTerrainDetail;
    ResetMesh(Out);
    const int32 N = Field.GetN();
    if (N < 2)
    {
        return;
    }
    Stride = FMath::Max(Stride, 1);
    TArray<int32> Samples;
    for (int32 K = 0; K < N; K += Stride)
    {
        Samples.Add(K);
    }
    if (Samples.Last() != N - 1)
    {
        Samples.Add(N - 1);
    }
    Out.Vertices.Reserve(Samples.Num() * Samples.Num());
    for (const int32 J : Samples)
    {
        for (const int32 I : Samples)
        {
            Out.Vertices.Add(LoomaWirePointToUe(FVector(Field.ToLocal(I), Field.At(I, J), Field.ToLocal(J))));
        }
    }
    AddGrid(Samples.Num(), Samples.Num(), Out.Triangles);
}
