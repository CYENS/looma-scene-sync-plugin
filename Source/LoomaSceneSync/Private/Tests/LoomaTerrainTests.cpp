#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaTerrain.h"
#include "LoomaTerrainActor.h"
#include "LoomaWireConvert.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "LoomaOceanActor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

/**
 * The scene's ground: the `terrain` component and an Ocean's floor
 * (looma-xr-asset-demo docs/scene-format.md, "The `terrain` component" and "The sea
 * floor"), parsed, chosen, decoded and drawn.
 */
namespace LoomaTerrainTests
{

FLoomaNodeComponents ParseTerrainJson(const FString& Json)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Values);
    return LoomaParseComponents(&Values);
}

/** A tile id that is only a name: 64 copies of one hex digit. */
FString FakeTileId(TCHAR Digit)
{
    return FString::ChrN(64, Digit);
}

/** A tile's bytes: `Height(i, j)` metres at each of its heights, encoded little-endian. */
TArray<uint8> EncodeTile(TFunctionRef<float(int32, int32)> Height)
{
    TArray<uint8> Bytes;
    Bytes.SetNumZeroed(LoomaTerrain::TileBytes);
    for (int32 J = 0; J < LoomaTerrain::Tile; ++J)
    {
        for (int32 I = 0; I < LoomaTerrain::Tile; ++I)
        {
            const int32 V = FMath::Clamp(FMath::RoundToInt32(Height(I, J) * 256.0f) + 32768, 0, 65535);
            Bytes[2 * (J * LoomaTerrain::Tile + I)] = static_cast<uint8>(V & 0xff);
            Bytes[2 * (J * LoomaTerrain::Tile + I) + 1] = static_cast<uint8>(V >> 8);
        }
    }
    return Bytes;
}

TArray<uint16> DecodeTile(const TArray<uint8>& Bytes)
{
    TArray<uint16> Out;
    Out.SetNum(LoomaTerrain::Tile * LoomaTerrain::Tile);
    for (int32 V = 0; V < Out.Num(); ++V)
    {
        Out[V] = static_cast<uint16>(Bytes[2 * V] | (Bytes[2 * V + 1] << 8));
    }
    return Out;
}

/** Unreal's front-face normal of triangle `K`: (P1 − P2) × (P0 − P2). */
FVector FrontFace(const FLoomaTerrainMesh& Mesh, int32 K)
{
    const FVector& P0 = Mesh.Vertices[Mesh.Triangles[3 * K]];
    const FVector& P1 = Mesh.Vertices[Mesh.Triangles[3 * K + 1]];
    const FVector& P2 = Mesh.Vertices[Mesh.Triangles[3 * K + 2]];
    return (P1 - P2) ^ (P0 - P2);
}
} // namespace LoomaTerrainTests

using namespace LoomaTerrainTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaTerrainParseTest, "Looma.Terrain.Parse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaTerrainParseTest::RunTest(const FString& Parameters)
{
    const FString A = FakeTileId('a');
    const FString B = FakeTileId('b');

    const FLoomaNodeComponents Full = ParseTerrainJson(FString::Printf(TEXT(R"([{"type":"terrain","size":120,"base":-3.5,
        "tiles":["%s",null,"not a tile","%s"],"priority":2}])"), *A, *B));
    TestTrue(TEXT("a terrain is read"), Full.bHasTerrain);
    TestFalse(TEXT("and is not an unknown type"), Full.UnknownTypes.Contains(TEXT("terrain")));
    TestFalse(TEXT("a node with a terrain is not empty"), Full.IsEmpty());
    TestEqual(TEXT("size"), Full.Terrain.Size, 120.0f);
    TestEqual(TEXT("base"), Full.Terrain.Base, -3.5f);
    TestEqual(TEXT("priority"), Full.Terrain.Priority, 2.0f);
    TestEqual(TEXT("four tiles are two across"), Full.Terrain.Across, 2);
    TestEqual(TEXT("ids kept, nulls and junk made null"), Full.Terrain.Tiles,
        TArray<FString>({A, FString(), FString(), B}));

    // The hub's clamps, made again on the way in.
    TestEqual(TEXT("size at least 10"), ParseTerrainJson(TEXT(R"([{"type":"terrain","size":5}])")).Terrain.Size, 10.0f);
    TestEqual(TEXT("size at most 2000"), ParseTerrainJson(TEXT(R"([{"type":"terrain","size":5000}])")).Terrain.Size, 2000.0f);
    TestEqual(TEXT("base at least -128"), ParseTerrainJson(TEXT(R"([{"type":"terrain","base":-500}])")).Terrain.Base, -128.0f);
    TestEqual(TEXT("base at most 127.99"), ParseTerrainJson(TEXT(R"([{"type":"terrain","base":500}])")).Terrain.Base, 127.99f);
    TestEqual(TEXT("a size nobody can read is the default"),
        ParseTerrainJson(TEXT(R"([{"type":"terrain","size":"100"}])")).Terrain.Size, 200.0f);

    // A list that is not square says nothing about where any tile goes.
    TArray<FString> AllNull;
    AllNull.Init(FString(), 64);
    for (const TCHAR* Tiles : {TEXT(""), TEXT(R"(,"tiles":null)"), TEXT(R"(,"tiles":[null,null,null])"),
             TEXT(R"(,"tiles":[])"), TEXT(R"(,"tiles":"abc")")})
    {
        const FLoomaTerrainSpec Flat = ParseTerrainJson(FString::Printf(TEXT(R"([{"type":"terrain"%s}])"), Tiles)).Terrain;
        TestEqual(FString::Printf(TEXT("'%s': eight across"), Tiles), Flat.Across, 8);
        TestTrue(FString::Printf(TEXT("'%s': all null"), Tiles), Flat.Tiles == AllNull);
    }
    FString Seventeen = TEXT("[");
    for (int32 K = 0; K < 17 * 17; ++K)
    {
        Seventeen += K ? TEXT(",null") : TEXT("null");
    }
    Seventeen += TEXT("]");
    TestEqual(TEXT("at most 16 across"),
        ParseTerrainJson(FString::Printf(TEXT(R"([{"type":"terrain","tiles":%s}])"), *Seventeen)).Terrain.Across, 8);

    TestFalse(TEXT("switched off: no terrain"),
        ParseTerrainJson(TEXT(R"([{"type":"terrain","enabled":false}])")).bHasTerrain);

    // The Ocean's sea state and floor.
    const FLoomaNodeComponents Sea = ParseTerrainJson(FString::Printf(TEXT(R"([{"type":"ocean","seabed":12,"floorSize":300,
        "wind":9,"windDir":45,"swell":1.25,"floor":["%s",null,null,null],"priority":-1}])"), *A));
    TestTrue(TEXT("an ocean is read"), Sea.bHasOcean);
    TestFalse(TEXT("and is not an unknown type"), Sea.UnknownTypes.Contains(TEXT("ocean")));
    TestEqual(TEXT("wind speed"), Sea.Ocean.Wind, 9.0f);
    TestEqual(TEXT("wind direction"), Sea.Ocean.WindDir, 45.0);
    TestEqual(TEXT("swell"), Sea.Ocean.Swell, 1.25f);
    TestEqual(TEXT("seabed"), Sea.Ocean.Seabed, 12.0f);
    TestEqual(TEXT("floor size"), Sea.Ocean.FloorSize, 300.0f);
    TestEqual(TEXT("priority"), Sea.Ocean.Priority, -1.0f);
    TestTrue(TEXT("a listed floor is shaped"), Sea.Ocean.bShaped);
    TestEqual(TEXT("two across"), Sea.Ocean.Across, 2);
    TestEqual(TEXT("its first tile"), Sea.Ocean.Floor[0], A);

    const FLoomaOceanSpec Unshaped = ParseTerrainJson(TEXT(R"([{"type":"ocean","seabed":0.2}])")).Ocean;
    TestFalse(TEXT("no floor: not shaped"), Unshaped.bShaped);
    TestEqual(TEXT("and flat everywhere"), Unshaped.Floor.Num(), 64);
    TestEqual(TEXT("seabed at least 1"), Unshaped.Seabed, 1.0f);
    TestEqual(TEXT("seabed at most 100"), ParseTerrainJson(TEXT(R"([{"type":"ocean","seabed":500}])")).Ocean.Seabed, 100.0f);
    const FLoomaNodeComponents Default = ParseTerrainJson(TEXT(R"([{"type":"ocean"}])"));
    TestEqual(TEXT("defaults: seabed"), Default.Ocean.Seabed, 8.0f);
    TestEqual(TEXT("defaults: floor size"), Default.Ocean.FloorSize, 200.0f);
    TestEqual(TEXT("defaults: wind speed"), Default.Ocean.Wind, 5.0f);
    TestEqual(TEXT("defaults: wind direction"), Default.Ocean.WindDir, 118.0);
    TestEqual(TEXT("defaults: swell"), Default.Ocean.Swell, 0.4f);
    const FLoomaNodeComponents Malformed = ParseTerrainJson(TEXT(R"([{"type":"ocean","wind":"9","windDir":null,"swell":true}])"));
    TestTrue(TEXT("unreadable sea state uses defaults"), Malformed == Default);

    const FLoomaOceanSpec High = ParseTerrainJson(TEXT(R"([{"type":"ocean","wind":20,"windDir":450,"swell":3}])")).Ocean;
    TestEqual(TEXT("wind at most 15"), High.Wind, 15.0f);
    TestEqual(TEXT("positive direction wraps"), High.WindDir, 90.0);
    TestEqual(TEXT("swell at most 2"), High.Swell, 2.0f);
    const FLoomaOceanSpec Low = ParseTerrainJson(TEXT(R"([{"type":"ocean","wind":-5,"windDir":-90,"swell":-1}])")).Ocean;
    TestEqual(TEXT("wind at least zero"), Low.Wind, 0.0f);
    TestEqual(TEXT("negative direction wraps"), Low.WindDir, 270.0);
    TestEqual(TEXT("swell at least zero"), Low.Swell, 0.0f);

    for (const TCHAR* Edit : {TEXT(R"("wind":9)"), TEXT(R"("windDir":45)"), TEXT(R"("swell":1.25)")})
    {
        const FLoomaNodeComponents Changed = ParseTerrainJson(FString::Printf(TEXT(R"([{"type":"ocean",%s}])"), Edit));
        TestTrue(FString::Printf(TEXT("%s changes the components"), Edit), !(Changed == Default));
    }

    TestTrue(TEXT("a tile id is 64 lower-case hex digits"), LoomaIsTerrainTileId(A));
    TestFalse(TEXT("not upper case"), LoomaIsTerrainTileId(FString::ChrN(64, 'A')));
    TestFalse(TEXT("not 63 digits"), LoomaIsTerrainTileId(FString::ChrN(63, 'a')));
    TestFalse(TEXT("not other letters"), LoomaIsTerrainTileId(FString::ChrN(64, 'g')));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaTerrainChooseTest, "Looma.Terrain.Choose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaTerrainChooseTest::RunTest(const FString& Parameters)
{
    const FLoomaNodeComponents Plain = ParseTerrainJson(TEXT(R"([{"type":"terrain","size":80,"base":1.3}])"));
    const FLoomaNodeComponents Raised = ParseTerrainJson(TEXT(R"([{"type":"terrain","priority":1}])"));
    const FLoomaNodeComponents Sea = ParseTerrainJson(TEXT(R"([{"type":"ocean","seabed":6,"floorSize":150,"priority":-5}])"));
    const FLoomaNodeComponents Off = ParseTerrainJson(TEXT(R"([{"type":"ocean","enabled":false}])"));
    const FLoomaNodeComponents Model = ParseTerrainJson(TEXT(R"([{"type":"model","assetId":"chair"}])"));

    FLoomaGround Ground;
    TestFalse(TEXT("nothing: no ground"), LoomaChooseGround({}, Ground));
    TestFalse(TEXT("no ground among models"), LoomaChooseGround({{TEXT("m"), &Model}}, Ground));

    TestTrue(TEXT("one terrain"), LoomaChooseGround({{TEXT("t"), &Plain}}, Ground));
    TestEqual(TEXT("its host"), Ground.NodeId, FString(TEXT("t")));
    TestFalse(TEXT("is not a sea floor"), Ground.bOcean);
    TestEqual(TEXT("its size"), Ground.Size, 80.0f);
    TestEqual(TEXT("its base, held to the tiles' 1/256 m"), Ground.Base, 333.0f / 256.0f);

    // A tie goes to the smallest id, whichever comes first.
    LoomaChooseGround({{TEXT("b"), &Plain}, {TEXT("a"), &Plain}}, Ground);
    TestEqual(TEXT("tie: the smallest id"), Ground.NodeId, FString(TEXT("a")));
    LoomaChooseGround({{TEXT("a"), &Plain}, {TEXT("b"), &Plain}}, Ground);
    TestEqual(TEXT("tie, the other order: the smallest id"), Ground.NodeId, FString(TEXT("a")));
    LoomaChooseGround({{TEXT("a"), &Plain}, {TEXT("b"), &Raised}}, Ground);
    TestEqual(TEXT("the higher priority"), Ground.NodeId, FString(TEXT("b")));

    // An Ocean's floor is the ground, whatever the priorities.
    TestTrue(TEXT("with an ocean"), LoomaChooseGround({{TEXT("a"), &Raised}, {TEXT("z"), &Sea}}, Ground));
    TestEqual(TEXT("the ocean's host"), Ground.NodeId, FString(TEXT("z")));
    TestTrue(TEXT("is a sea floor"), Ground.bOcean);
    TestEqual(TEXT("as wide as its floor"), Ground.Size, 150.0f);
    TestEqual(TEXT("unshaped at the seabed's depth"), Ground.Base, -6.0f);
    TestEqual(TEXT("eight tiles across"), Ground.Across, 8);

    LoomaChooseGround({{TEXT("a"), &Plain}, {TEXT("z"), &Off}}, Ground);
    TestEqual(TEXT("an ocean switched off is no ground"), Ground.NodeId, FString(TEXT("a")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaTerrainHeightsTest, "Looma.Terrain.Heights",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaTerrainHeightsTest::RunTest(const FString& Parameters)
{
    // backend/app/terrain.py: v is (v − 32768) / 256 metres.
    TestEqual(TEXT("32768 is the host's height"), LoomaTerrain::DecodeHeight(32768), 0.0f);
    TestEqual(TEXT("256 steps a metre"), LoomaTerrain::DecodeHeight(33024), 1.0f);
    TestEqual(TEXT("the lowest"), LoomaTerrain::DecodeHeight(0), -128.0f);
    TestEqual(TEXT("the highest"), LoomaTerrain::DecodeHeight(65535), 32767.0f / 256.0f);
    TestEqual(TEXT("quantized to 1/256 m"), LoomaTerrain::Quantize(1.3f), 333.0f / 256.0f);
    TestEqual(TEXT("and clamped"), LoomaTerrain::Quantize(500.0f), 32767.0f / 256.0f);

    FLoomaHeightfield Field;
    Field.Reset(2, 100.0f, 1.3f);
    TestEqual(TEXT("64 heights a tile"), Field.GetN(), 128);
    TestEqual(TEXT("the first height at the square's edge"), Field.ToLocal(0), -50.0f);
    TestEqual(TEXT("the last at the other"), Field.ToLocal(127), 50.0f, 1e-4f);
    TestEqual(TEXT("everything starts at the base"), Field.At(77, 101), 333.0f / 256.0f);

    // LoomaTerrain::Tile 1 is the second along +x: heights i metres, rising along +x.
    const TArray<uint16> Ramp = DecodeTile(EncodeTile([](int32 I, int32) { return static_cast<float>(I); }));
    Field.FillTile(1, Ramp.GetData());
    TestEqual(TEXT("tile 1 starts at column 64"), Field.At(64 + 5, 3), 5.0f);
    TestEqual(TEXT("tile 0 is untouched"), Field.At(5, 3), 333.0f / 256.0f);
    TestEqual(TEXT("tile 3 is untouched"), Field.At(70, 70), 333.0f / 256.0f);
    TestEqual(TEXT("out of the square: the nearest height"), Field.At(500, 3), 63.0f);
    TestEqual(TEXT("the lowest"), Field.Lowest(), 0.0f);
    Field.FillTile(1, nullptr);
    TestEqual(TEXT("a null tile is flat at the base"), Field.At(64 + 5, 3), 333.0f / 256.0f);

    // Between heights, interpolated as the web's terrainGround is.
    Field.FillTile(1, Ramp.GetData());
    float Height = 0.0f;
    TestTrue(TEXT("on a height"), Field.HeightAt(Field.ToLocal(66), Field.ToLocal(2), Height));
    TestEqual(TEXT("is that height"), Height, 2.0f, 1e-3f);
    TestTrue(TEXT("between two"), Field.HeightAt(0.5f * (Field.ToLocal(66) + Field.ToLocal(67)), Field.ToLocal(2), Height));
    TestEqual(TEXT("is halfway"), Height, 2.5f, 1e-3f);
    TestFalse(TEXT("off the square"), Field.HeightAt(50.1f, 0.0f, Height));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaTerrainMeshTest, "Looma.Terrain.Mesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaTerrainMeshTest::RunTest(const FString& Parameters)
{
    FLoomaHeightfield Field;
    Field.Reset(2, 100.0f, 0.0f);
    FLoomaTerrainMesh Mesh;

    // Sections overlap by a row and a column, so neighbours share their edge.
    LoomaBuildTileMesh(Field, 0, false, true, Mesh);
    TestEqual(TEXT("tile 0 reaches into tiles 1 and 2"), Mesh.Vertices.Num(), 65 * 65);
    TestEqual(TEXT("two triangles a cell"), Mesh.Triangles.Num(), 64 * 64 * 6);
    TestTrue(TEXT("its first height stands at the square's corner"),
        Mesh.Vertices[0].Equals(LoomaWirePointToUe(FVector(-50, 0, -50)), 1e-3));
    const FVector Corner = Mesh.Vertices[64];
    LoomaBuildTileMesh(Field, 1, false, true, Mesh);
    TestEqual(TEXT("the last tile along x stops at the edge"), Mesh.Vertices.Num(), 64 * 65);
    TestTrue(TEXT("and starts where tile 0 ends"), Mesh.Vertices[0].Equals(Corner, 1e-3));
    LoomaBuildTileMesh(Field, 3, false, true, Mesh);
    TestEqual(TEXT("the last tile"), Mesh.Vertices.Num(), 64 * 64);
    LoomaBuildTileMesh(Field, 0, false, false, Mesh);
    TestEqual(TEXT("an update has no triangles"), Mesh.Triangles.Num(), 0);
    LoomaBuildTileMesh(Field, 4, false, true, Mesh);
    TestEqual(TEXT("no such tile: nothing"), Mesh.Vertices.Num(), 0);

    // Wire +x (column) is Unreal +Y and wire +z (row) is Unreal −X.
    LoomaBuildTileMesh(Field, 0, false, true, Mesh);
    TestTrue(TEXT("along a row is Unreal +Y"), (Mesh.Vertices[1] - Mesh.Vertices[0]).GetSafeNormal().Equals(FVector(0, 1, 0), 1e-6));
    TestTrue(TEXT("down a column is Unreal -X"), (Mesh.Vertices[65] - Mesh.Vertices[0]).GetSafeNormal().Equals(FVector(-1, 0, 0), 1e-6));
    TestTrue(TEXT("flat ground faces up"), Mesh.Normals[100].Equals(FVector(0, 0, 1), 1e-6));

    // Every triangle faces up, even down a 31 m cliff where tile 0 meets flat tile 1.
    const TArray<uint16> Ramp = DecodeTile(EncodeTile([](int32 I, int32 J) { return 0.5f * I + 0.2f * J; }));
    Field.FillTile(0, Ramp.GetData());
    LoomaBuildTileMesh(Field, 0, false, true, Mesh);
    int32 Down = 0;
    for (int32 K = 0; K < Mesh.Triangles.Num() / 3; ++K)
    {
        Down += FrontFace(Mesh, K).Z <= 0.0 ? 1 : 0;
    }
    TestEqual(TEXT("no triangle faces down"), Down, 0);
    // Rising along wire +x (Unreal +Y): the normal leans back towards −Y.
    TestTrue(TEXT("a slope's normal leans away from uphill"), Mesh.Normals[65 * 10 + 10].Y < -0.1);

    // On a slope that runs on across every tile, each face agrees with its vertex normals.
    // (Beside a cliff it need not: there the web's normal, taken from the heights either
    // side, is nearly level, and so is this one.)
    for (int32 T = 0; T < 4; ++T)
    {
        const int32 OX = (T % 2) * LoomaTerrain::Tile;
        const int32 OZ = (T / 2) * LoomaTerrain::Tile;
        const TArray<uint16> Slope = DecodeTile(EncodeTile([OX, OZ](int32 I, int32 J)
            { return 0.3f * (I + OX) - 0.2f * (J + OZ); }));
        Field.FillTile(T, Slope.GetData());
    }
    int32 Against = 0;
    for (int32 T = 0; T < 4; ++T)
    {
        LoomaBuildTileMesh(Field, T, false, true, Mesh);
        for (int32 K = 0; K < Mesh.Triangles.Num() / 3; ++K)
        {
            for (int32 Vertex = 0; Vertex < 3; ++Vertex)
            {
                Against += (FrontFace(Mesh, K) | Mesh.Normals[Mesh.Triangles[3 * K + Vertex]]) <= 0.0 ? 1 : 0;
            }
        }
    }
    TestEqual(TEXT("no face against its vertex normals"), Against, 0);

    // Grass on flat land, rock on a cliff, sand at the waterline.
    const FLinearColor Grass = LoomaGroundColor(3, 7, 5, FVector(0, 1, 0), false);
    TestTrue(TEXT("flat land is green"), Grass.G > Grass.R && Grass.G > Grass.B);
    const FLinearColor Cliff = LoomaGroundColor(3, 7, 5, FVector(0.8, 0.6, 0).GetSafeNormal(), false);
    TestTrue(TEXT("a cliff is grey rock"), FMath::Abs(Cliff.R - Cliff.G) < 0.05f && Cliff.R > Cliff.B);
    const FLinearColor Beach = LoomaGroundColor(3, 7, 0.2f, FVector(0, 1, 0), true);
    TestTrue(TEXT("by the water it is sand"), Beach.R > Beach.G && Beach.G > Beach.B);
    const FLinearColor Dry = LoomaGroundColor(3, 7, 0.2f, FVector(0, 1, 0), false);
    TestTrue(TEXT("with no water there is no beach"), Dry.G > Dry.R);

    // The skirt hangs from every edge height to the bottom, facing out: below the lowest
    // ground, and also where a seabed above some of it makes it rise.
    for (const float Bottom : {-40.0f, 5.5f})
    {
        LoomaBuildSkirtMesh(Field, Bottom, Mesh);
        const FString Label = FString::Printf(TEXT("skirt to %.1f m: "), Bottom);
        TestEqual(Label + TEXT("two vertices for every edge height"), Mesh.Vertices.Num(), 4 * 128 * 2);
        TestEqual(Label + TEXT("its bottom"), Mesh.Vertices[1].Z, Bottom * 100.0, 1e-3);
        int32 Inward = 0;
        for (int32 K = 0; K < Mesh.Triangles.Num() / 3; ++K)
        {
            Inward += (FrontFace(Mesh, K) | Mesh.Normals[Mesh.Triangles[3 * K]]) < 0.0 ? 1 : 0;
        }
        TestEqual(Label + TEXT("every face of it faces out"), Inward, 0);
    }

    LoomaBuildCollisionMesh(Field, 2, Mesh);
    TestEqual(TEXT("collision: every other height and the last"), Mesh.Vertices.Num(), 65 * 65);
    TestEqual(TEXT("collision triangles"), Mesh.Triangles.Num(), 64 * 64 * 6);
    return true;
}

/** A subsystem with nodes seeded straight into it, fed the hub's messages. */
struct FLoomaTerrainTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};
    UWorld* World = nullptr;
    int32 NextOrder = 0;

    FLoomaTerrainTestAccess()
    {
        const UWorld::InitializationValues Values = UWorld::InitializationValues()
            .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
    }

    ~FLoomaTerrainTestAccess()
    {
        Sync->Tracked.Reset();
        Sync->TerrainActor = nullptr;
        Sync->OceanActor = nullptr;
        World->DestroyWorld(false);
    }

    ALoomaSyncedActor* Seed(const TCHAR* Id, const FTransform& Local)
    {
        ALoomaSyncedActor* Actor = World->SpawnActor<ALoomaSyncedActor>();
        Actor->Id = Id;
        Actor->DisplayName = Id;
        Actor->SetLocalTransform(Local);
        FLoomaTrackedActor Entry;
        Entry.Actor = Actor;
        Entry.Order = NextOrder++;
        Sync->Tracked.Add(Id, Entry);
        return Actor;
    }

    void Patch(const TCHAR* Id, const FString& Components)
    {
        Sync->OnRawMessage(FString::Printf(TEXT(R"({"type":"patch","nodes":[{"id":"%s","components":%s}]})"), Id, *Components));
    }

    /** What the subsystem's Tick does for the ground and Ocean. */
    void Frame()
    {
        Sync->RefreshSkybox();
        Sync->RefreshGround();
        Sync->TickOcean();
    }

    ALoomaTerrainActor* Terrain() const { return Sync->GetTerrainActor(); }
    ALoomaOceanActor* Ocean() const { return Sync->OceanActor.Get(); }
    const AActor* OceanHost() const { return Ocean()->OceanHost.Get(); }
    UProceduralMeshComponent* OceanMesh() const { return Ocean()->Mesh.Get(); }
    UMaterialInstanceDynamic* OceanSurface() const { return Ocean()->Surface.Get(); }
    const UTexture2D* OceanFloor() const { return Ocean()->FloorTexture.Get(); }
    uint64 OceanFloorRevision() const { return Ocean()->FloorRevision; }
    void TickOcean() { Sync->TickOcean(); }
    void Send(const FString& Message) { Sync->OnRawMessage(Message); }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaOceanSceneTest, "Looma.Ocean.Scene",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaOceanSceneTest::RunTest(const FString& Parameters)
{
    FLoomaTerrainTestAccess Access;
    ALoomaSyncedActor* Parent = Access.Seed(TEXT("parent"), FTransform(FVector(0, 0, 100)));
    ALoomaSyncedActor* Sea = Access.Seed(TEXT("sea"), FTransform(FVector(0, 0, 200)));
    ALoomaSyncedActor* Other = Access.Seed(TEXT("other"), FTransform(FVector(0, 0, 900)));
    Access.Send(TEXT(R"({"type":"reparent","nodes":[{"id":"sea","parent":"parent"}]})"));
    Access.Patch(TEXT("sea"), TEXT(R"([{"type":"ocean","priority":5,"wind":0,"swell":0}])"));
    Access.Patch(TEXT("other"), TEXT(R"([{"type":"ocean","priority":4}])"));
    Access.Frame();

    ALoomaOceanActor* Ocean = Access.Ocean();
    if (!TestNotNull(TEXT("the active Ocean creates a water actor"), Ocean)
        || !TestNotNull(TEXT("the cooked material creates a runtime instance"), Access.OceanSurface()))
    {
        return false;
    }
    TestTrue(TEXT("the water is shown"), !Access.OceanMesh()->bHiddenInGame);
    TestEqual(TEXT("one water section"), Access.OceanMesh()->GetNumSections(), 1);
    for (int32 Band = 0; Band < FLoomaOceanWaves::BandCount; ++Band)
    {
        UTexture* Parameter = Access.OceanSurface()->K2_GetTextureParameterValue(
            FName(*FString::Printf(TEXT("Wave%d"), Band)));
        TestNotNull(TEXT("the packaged material declares every displacement texture"), Parameter);
        if (FApp::CanEverRender())
        {
            const UTexture2D* Texture = Cast<UTexture2D>(Parameter);
            if (TestNotNull(TEXT("the material uses a runtime displacement texture"), Texture))
            {
                TestEqual(TEXT("the wave texture includes all eight mip levels"), Texture->GetNumMips(), 8);
                TestEqual(TEXT("the wave texture filters between mip levels"), Texture->Filter.GetValue(), TF_Trilinear);
            }
        }
    }
    const FProcMeshSection* Section = Access.OceanMesh()->GetProcMeshSection(0);
    if (TestNotNull(TEXT("the water has a mesh section"), Section) && !Section->ProcVertexBuffer.IsEmpty())
    {
        const TArray<FProcMeshVertex>& Vertices = Section->ProcVertexBuffer;
        TestEqual(TEXT("the central vertex reports one metre of spacing"), Vertices[Vertices.Num() / 2].UV0.X, 1.0);
        TestTrue(TEXT("outer vertices report the wider spacing"), Vertices[0].UV0.X > 1.0);
    }
    TestEqual(TEXT("water is visual only"), Access.OceanMesh()->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
    TestEqual(TEXT("the mesh uses the water instance"), Access.OceanMesh()->GetMaterial(0),
        static_cast<UMaterialInterface*>(Access.OceanSurface()));
    TestEqual(TEXT("the winner also owns the floor"), Access.Terrain()->GetHostId(), FString(TEXT("sea")));
    TestTrue(TEXT("the water follows the winning host"), Access.OceanHost() == Sea);
    TestTrue(TEXT("the host is parented"), Sea->GetAttachParentActor() == Parent);
    TestEqual(TEXT("sea level follows the host's world Z"), Ocean->GetActorLocation().Z, 300.0);
    TestEqual(TEXT("calm wind"), Access.OceanSurface()->K2_GetVectorParameterValue(TEXT("Wind")).B, 0.0f);
    TestEqual(TEXT("calm swell"), Access.OceanSurface()->K2_GetVectorParameterValue(TEXT("Wind")).A, 0.0f);

    const double BeforeMove = Ocean->GetActorLocation().Z;
    Parent->AddActorWorldOffset(FVector(0, 0, 175));
    Access.TickOcean();
    TestEqual(TEXT("moving an ancestor updates sea level without a component patch"), Ocean->GetActorLocation().Z, BeforeMove + 175.0);

    const FString CoastId = FakeTileId('c');
    Access.Patch(TEXT("sea"), FString::Printf(TEXT(R"([{"type":"ocean","priority":5,"wind":10,"windDir":90,"swell":1.25,
        "floorSize":60,"floor":["%s"]}])"), *CoastId));
    Access.Frame();
    TestTrue(TEXT("live edits retain the same renderer"), Access.Ocean() == Ocean);
    const FLinearColor Wind = Access.OceanSurface()->K2_GetVectorParameterValue(TEXT("Wind"));
    TestTrue(TEXT("wind settings reach the material in UE axes"), Wind.Equals(FLinearColor(-1, 0, 10, 1.25f)));
    const uint64 BeforeTile = Access.OceanFloorRevision();
    const UTexture2D* BeforeTexture = Access.OceanFloor();
    const TArray<uint8> Coast = EncodeTile([](int32 I, int32) { return I < 32 ? -1.0f : 1.0f; });
    Access.Terrain()->AddTiles({CoastId}, Coast);
    Access.TickOcean();
    TestTrue(TEXT("the water reads the new rendered heights"), Access.OceanFloorRevision() > BeforeTile);
    TestTrue(TEXT("the same-resolution edit reuses the height texture"), Access.OceanFloor() == BeforeTexture);
    TestEqual(TEXT("floor texture resolution reaches the material"), Access.OceanSurface()->K2_GetVectorParameterValue(TEXT("Floor")).A, 64.0f);
    if (TestNotNull(TEXT("a shaped Ocean uploads its terrain field"), Access.OceanFloor()))
    {
        TestEqual(TEXT("one tile's heights a side"), Access.OceanFloor()->GetSizeX(), 64);
    }

    Access.Patch(TEXT("parent"), TEXT(R"([{"type":"hidden"}])"));
    Access.Frame();
    TestTrue(TEXT("a hidden ancestor hands the Ocean to the next host"), Access.OceanHost() == Other);
    Access.Patch(TEXT("parent"), TEXT("[]"));
    Access.Patch(TEXT("sea"), TEXT(R"([{"type":"ocean","priority":5,"enabled":false}])"));
    Access.Frame();
    TestTrue(TEXT("a disabled Ocean also yields"), Access.OceanHost() == Other);
    Access.Patch(TEXT("other"), TEXT("[]"));
    Access.Frame();
    TestTrue(TEXT("removing the last enabled Ocean hides the water"), Access.OceanMesh()->bHiddenInGame);
    Access.Patch(TEXT("sea"), TEXT(R"([{"type":"ocean"}])"));
    Access.Frame();
    TestTrue(TEXT("a new active Ocean reuses the water"), !Access.OceanMesh()->bHiddenInGame);
    Access.Send(TEXT(R"({"type":"scene","sceneId":"empty","nodes":[]})"));
    Access.Frame();
    TestTrue(TEXT("a replacement scene without Ocean hides the water"), Access.OceanMesh()->bHiddenInGame);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaTerrainSceneTest, "Looma.Terrain.Scene",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaTerrainSceneTest::RunTest(const FString& Parameters)
{
    FLoomaTerrainTestAccess Access;
    // A host turned, scaled and placed: the ground takes only its position.
    const FTransform HostPose(FQuat(FVector(1, 1, 0).GetSafeNormal(), 0.7), LoomaWirePointToUe(FVector(10, 2, -5)),
        FVector(3, 0.5, 2));
    ALoomaSyncedActor* Host = Access.Seed(TEXT("ground"), HostPose);

    // --- A flat terrain ----------------------------------------------------------------
    Access.Patch(TEXT("ground"), TEXT(R"([{"type":"terrain","size":100,"tiles":[null,null,null,null]}])"));
    Access.Frame();
    ALoomaTerrainActor* Terrain = Access.Terrain();
    if (!TestNotNull(TEXT("a terrain makes the ground actor"), Terrain))
    {
        return false;
    }
    TestTrue(TEXT("showing"), Terrain->IsShowing());
    TestEqual(TEXT("on its host"), Terrain->GetHostId(), FString(TEXT("ground")));
    TestEqual(TEXT("attached to the host"), Terrain->GetAttachParentActor(), static_cast<AActor*>(Host));
    TestTrue(TEXT("standing on the host's position"), Terrain->GetActorLocation().Equals(Host->GetActorLocation(), 1e-3));
    TestTrue(TEXT("level, whatever the host's turn"), Terrain->GetActorQuat().Equals(FQuat::Identity, 1e-6));
    TestTrue(TEXT("unscaled, whatever the host's scale"), Terrain->GetActorScale3D().Equals(FVector::OneVector, 1e-6));
    TestEqual(TEXT("a section a tile, and the skirt"), Terrain->GetMesh()->GetNumSections(), 5);
    TestTrue(TEXT("its collision waits for the ground to settle"), Terrain->IsCollisionPending());
    float Z = 0.0f;
    TestTrue(TEXT("ground under the host"), Terrain->GetGroundHeight(Host->GetActorLocation(), Z));
    TestEqual(TEXT("at the host's height"), static_cast<double>(Z), Host->GetActorLocation().Z, 1e-2);
    TestFalse(TEXT("none past the edge"), Terrain->GetGroundHeight(Host->GetActorLocation() + FVector(0, 5100, 0), Z));

    // --- Sculpted: two tiles change ids ------------------------------------------------
    // The tiles arrive as a fetch would hand them over, so no backend is needed.
    const FString Plateau = FakeTileId('a');
    const FString Ramp = FakeTileId('b');
    TArray<uint8> Bytes = EncodeTile([](int32, int32) { return 2.0f; });
    Bytes.Append(EncodeTile([](int32 I, int32) { return 0.1f * I; }));
    Terrain->AddTiles({Plateau, Ramp}, Bytes);
    TestEqual(TEXT("both tiles kept"), Terrain->GetKeptTiles(), 2);
    Access.Patch(TEXT("ground"), FString::Printf(TEXT(R"([{"type":"terrain","size":100,"tiles":["%s",null,null,"%s"]}])"),
        *Plateau, *Ramp));
    Access.Frame();
    TestEqual(TEXT("the same actor"), Access.Terrain(), Terrain);
    TestEqual(TEXT("nothing is waiting"), Terrain->GetWaitingTiles(), 0);
    TestEqual(TEXT("nothing was fetched"), Terrain->GetTilesInFlight(), 0);
    const FVector Origin = Host->GetActorLocation();
    TestTrue(TEXT("ground over tile 0"), Terrain->GetGroundHeight(Origin + LoomaWirePointToUe(FVector(-40, 0, -40)), Z));
    TestEqual(TEXT("is the plateau"), static_cast<double>(Z), Origin.Z + 200.0, 1e-2);
    const FProcMeshSection* First = Terrain->GetMesh()->GetProcMeshSection(0);
    if (TestNotNull(TEXT("tile 0's section"), First))
    {
        TestEqual(TEXT("its vertices stand on the plateau"), First->ProcVertexBuffer[10].Position.Z, 200.0, 1e-3);
    }
    const FProcMeshSection* Last = Terrain->GetMesh()->GetProcMeshSection(3);
    if (TestNotNull(TEXT("tile 3's section"), Last))
    {
        TestEqual(TEXT("its vertices climb the ramp"), Last->ProcVertexBuffer[10].Position.Z, 100.0, 1e-3);
    }
    TestEqual(TEXT("still a section a tile, and the skirt"), Terrain->GetMesh()->GetNumSections(), 5);

    // --- An Ocean: its floor is the ground ----------------------------------------------
    ALoomaSyncedActor* Water = Access.Seed(TEXT("sea"), FTransform(LoomaWirePointToUe(FVector(0, 1, 0))));
    Access.Patch(TEXT("sea"), TEXT(R"([{"type":"ocean","seabed":6,"floorSize":60}])"));
    Access.Frame();
    TestEqual(TEXT("the ocean holds the ground"), Terrain->GetHostId(), FString(TEXT("sea")));
    TestTrue(TEXT("a sea floor"), Terrain->IsSeaFloor());
    TestEqual(TEXT("attached to the ocean"), Terrain->GetAttachParentActor(), static_cast<AActor*>(Water));
    TestEqual(TEXT("unshaped: one section a tile of eight across, and the skirt"),
        Terrain->GetMesh()->GetNumSections(), 65);
    TestTrue(TEXT("ground under the water"), Terrain->GetGroundHeight(Water->GetActorLocation(), Z));
    TestEqual(TEXT("at the seabed's depth"), static_cast<double>(Z), Water->GetActorLocation().Z - 600.0, 1e-2);

    Access.Patch(TEXT("sea"), TEXT(R"([{"type":"ocean","seabed":6,"enabled":false}])"));
    Access.Frame();
    TestEqual(TEXT("ocean off: the terrain again"), Terrain->GetHostId(), FString(TEXT("ground")));
    TestEqual(TEXT("its sculpted tiles drawn from those kept"), Terrain->GetWaitingTiles(), 0);
    TestTrue(TEXT("the plateau again"), Terrain->GetGroundHeight(Origin + LoomaWirePointToUe(FVector(-40, 0, -40)), Z)
        && FMath::IsNearlyEqual(static_cast<double>(Z), Origin.Z + 200.0, 1e-2));

    // --- Hidden, or switched off: no ground ---------------------------------------------
    Access.Patch(TEXT("ground"), TEXT(R"([{"type":"terrain","size":100},{"type":"hidden"}])"));
    Access.Frame();
    TestFalse(TEXT("a hidden host holds no ground"), Terrain->IsShowing());
    TestEqual(TEXT("and nothing is drawn"), Terrain->GetMesh()->GetNumSections(), 0);
    TestNull(TEXT("and it lets go of the host"), Terrain->GetAttachParentActor());
    Access.Patch(TEXT("ground"), TEXT(R"([{"type":"terrain","size":100}])"));
    Access.Frame();
    TestTrue(TEXT("shown again"), Terrain->IsShowing());
    Access.Patch(TEXT("ground"), TEXT(R"([{"type":"terrain","size":100,"enabled":false}])"));
    Access.Frame();
    TestFalse(TEXT("a terrain switched off is no ground"), Terrain->IsShowing());
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
