#include "LoomaSyncedActor.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Scene.h"
#include "Engine/StaticMesh.h"
#include "LoomaSceneSyncLog.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"

namespace
{
/** Wire metres → UE centimetres. Matches glTFRuntime's default SceneScale. */
constexpr double WireToCm = 100.0;

/**
 * Engine primitives back the `mesh` component — a box, sphere, plane and cylinder
 * are exactly what /Engine/BasicShapes provides, and no procedural geometry has to
 * be generated or shipped.
 *
 * These are loaded by path at runtime, so the cooker cannot see the reference: a
 * **packaged** build needs `/Engine/BasicShapes` in Project Settings → Packaging →
 * *Additional Asset Directories to Cook*. In the editor and in PIE they are always
 * available. Nothing else in the plugin depends on cooked content.
 */
const TCHAR* PrimitiveAssetPath(ELoomaMeshShape Shape)
{
    switch (Shape)
    {
    case ELoomaMeshShape::Sphere:   return TEXT("/Engine/BasicShapes/Sphere.Sphere");
    case ELoomaMeshShape::Plane:    return TEXT("/Engine/BasicShapes/Plane.Plane");
    case ELoomaMeshShape::Cylinder: return TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
    case ELoomaMeshShape::Box:
    default:                        return TEXT("/Engine/BasicShapes/Cube.Cube");
    }
}

/**
 * Base material for procedural meshes: the engine's own basic-shape material, which
 * exposes a `Color` vector parameter and a `Roughness` scalar — enough for the
 * `material` component. `metalness` has no parameter to drive here, so it is parsed
 * and then ignored rather than faked; a GLB is the route to a fully authored surface.
 */
const TCHAR* PrimitiveMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
const TCHAR* ColorParameter = TEXT("Color");
const TCHAR* RoughnessParameter = TEXT("Roughness");

/**
 * three.js reads `distance: 0` as "no falloff limit"; UE always clamps a local light
 * at its attenuation radius. 10 m is a stage-scale stand-in for unlimited — big
 * enough not to cut a practical off mid-set, small enough to stay cheap.
 */
constexpr float UnlimitedAttenuationCm = 1000.0f;

/**
 * The extent a `mesh` should end up with, in UE axes and centimetres.
 *
 * `size` is an **extent in metres**, not a scale factor, so each shape reads the same
 * components the web renderer reads
 * (looma-xr-asset-demo/frontend/src/scene/components/mesh.jsx) — otherwise `[1,1,1]`
 * would mean two different objects in the two clients. A non-positive component means
 * "leave that axis alone", which is how a plane's normal axis is handled.
 */
FVector PrimitiveExtentCm(const FLoomaMeshSpec& Spec)
{
    const FVector S = Spec.Size; // wire axes, metres
    switch (Spec.Shape)
    {
    // The web builds a sphere from `size.x` alone (radius x/2), so it stays a sphere
    // however the other two components were authored.
    case ELoomaMeshShape::Sphere:
        return FVector(S.X, S.X, S.X) * WireToCm;
    // Radius from `size.x`, height from `size.y`. UE's cylinder runs along +Z, which
    // is where wire +Y (up) lands.
    case ELoomaMeshShape::Cylinder:
        return FVector(S.X, S.X, S.Y) * WireToCm;
    // A plane is `size.x` by `size.z` and horizontal in both clients. UE's plane is
    // already in the XY ground plane, so — unlike three.js, which has to rotate an
    // upright XY plane flat — no extra rotation is needed here.
    case ELoomaMeshShape::Plane:
        return FVector(S.Z * WireToCm, S.X * WireToCm, 0.0);
    // The full change of basis: wire (x, y, z) → UE (−z, x, y). An extent is
    // unsigned, so only the permutation survives.
    case ELoomaMeshShape::Box:
    default:
        return FVector(S.Z, S.X, S.Y) * WireToCm;
    }
}

UClass* LightClassFor(ELoomaLightType Type)
{
    switch (Type)
    {
    case ELoomaLightType::Spot:        return USpotLightComponent::StaticClass();
    case ELoomaLightType::Directional: return UDirectionalLightComponent::StaticClass();
    case ELoomaLightType::Point:
    default:                           return UPointLightComponent::StaticClass();
    }
}

template <typename T>
void DestroySlot(TObjectPtr<T>& Slot)
{
    if (T* Component = Slot.Get())
    {
        Component->DestroyComponent();
    }
    Slot = nullptr;
}
} // namespace

ALoomaSyncedActor::ALoomaSyncedActor()
{
    PrimaryActorTick.bCanEverTick = true;

    // A plain scene component, deliberately: the actor *is* a transform, and what it
    // renders hangs off this as children. While the root was the static mesh there was
    // no way to express an empty object and nowhere to hang a second component.
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SceneRoot->SetMobility(EComponentMobility::Movable);
    RootComponent = SceneRoot;
}

// --- Components ---------------------------------------------------------------

void ALoomaSyncedActor::ApplyComponents(const FLoomaNodeComponents& Next, const FLoomaNodeRenderContext& Context)
{
    ApplyModel(Next, Context);
    ApplyMesh(Next);
    ApplyMaterial(Next);
    ApplyLight(Next, Context);

    AssetId = Next.bHasModel ? Next.Model.AssetId : FString();
    if (Next.bHasModel && !Next.Model.JobId.IsEmpty())
    {
        JobId = Next.Model.JobId;
    }

    // Say so once per type, then carry on: the node itself is kept, geometry and all,
    // which is what lets the backend and the web client ship a component type this
    // build has never heard of.
    for (const FString& Unknown : Next.UnknownTypes)
    {
        if (!Components.UnknownTypes.Contains(Unknown))
        {
            UE_LOG(LogLoomaSync, Log,
                TEXT("Node '%s' carries component type '%s', which this build does not render — node kept."),
                *Id, *Unknown);
        }
    }

    Components = Next;
}

void ALoomaSyncedActor::ApplyModel(const FLoomaNodeComponents& Next, const FLoomaNodeRenderContext& Context)
{
    bBaseAlignModel = Context.bBaseAlignModels;

    if (!Next.bHasModel || Next.Model.AssetId.IsEmpty() || Context.ModelUrl.IsEmpty())
    {
        // No `model`, or one with no assetId: no component, no request, no 404. The
        // pre-v3 code spawned a mesh for every node and fetched
        // http://<host>/static/.glb for the ones with no asset, logging an error each
        // time — which is exactly what an empty object used to look like.
        DestroySlot(ModelComponent);
        LoadedModelUrl.Reset();
        return;
    }

    if (!EnsureMeshComponent(ModelComponent))
    {
        return;
    }
    if (Context.ModelUrl != LoadedModelUrl)
    {
        LoadedModelUrl = Context.ModelUrl;
        LoadMeshFromUrl(Context.ModelUrl);
    }
}

void ALoomaSyncedActor::ApplyMesh(const FLoomaNodeComponents& Next)
{
    if (!Next.bHasMesh)
    {
        DestroySlot(MeshComponent);
        MeshMaterial = nullptr;
        return;
    }

    UStaticMeshComponent* Component = EnsureMeshComponent(MeshComponent);
    if (!Component)
    {
        return;
    }

    if (!Components.bHasMesh || Components.Mesh.Shape != Next.Mesh.Shape || !Component->GetStaticMesh())
    {
        UStaticMesh* Primitive = LoadObject<UStaticMesh>(nullptr, PrimitiveAssetPath(Next.Mesh.Shape));
        if (!Primitive)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Node '%s': primitive '%s' failed to load (is /Engine/BasicShapes cooked?)"),
                *Id, PrimitiveAssetPath(Next.Mesh.Shape));
            return;
        }
        Component->SetStaticMesh(Primitive);
    }

    UStaticMesh* Built = Component->GetStaticMesh();
    if (!Built)
    {
        return;
    }

    // Fit the primitive to the authored extent by measuring it, rather than trusting
    // that every engine basic shape is 100 cm across. Self-correcting if one isn't.
    const FBox Box = Built->GetBoundingBox();
    const FVector BoxSize = Box.GetSize();
    const FVector Desired = PrimitiveExtentCm(Next.Mesh);
    FVector Scale = FVector::OneVector;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (Desired[Axis] > 0.0 && BoxSize[Axis] > UE_KINDA_SMALL_NUMBER)
        {
            Scale[Axis] = Desired[Axis] / BoxSize[Axis];
        }
    }
    Component->SetRelativeScale3D(Scale);
    // **Pivot is the node origin, not the base** — the same rule the web states
    // outright: a box created at y = 0 sits half in the floor, because a procedural
    // box's pivot *is* its centre. GLBs differ (see OnGlbLoaded) only because their
    // own pivots are arbitrary. Engine primitives are already centred, so this line
    // only corrects an asset that isn't.
    Component->SetRelativeLocation(-Box.GetCenter() * Scale);
}

void ALoomaSyncedActor::ApplyMaterial(const FLoomaNodeComponents& Next)
{
    // `material` applies to a sibling `mesh` only. A `model` carries its own materials
    // from inside the GLB, so a material beside one is ignored by every client —
    // overriding a GLB's surface would be a separate feature.
    UStaticMeshComponent* Component = MeshComponent.Get();
    if (!Component)
    {
        MeshMaterial = nullptr;
        return;
    }

    // Absent ⇒ the spec's defaults, which are the web's default grey: a mesh with no
    // material still has to be visible.
    const FLoomaMaterialSpec Spec = Next.bHasMaterial ? Next.Material : FLoomaMaterialSpec();

    if (!MeshMaterial)
    {
        UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, PrimitiveMaterialPath);
        if (!Base)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Node '%s': base material '%s' failed to load — the mesh keeps the engine default."),
                *Id, PrimitiveMaterialPath);
            return;
        }
        MeshMaterial = UMaterialInstanceDynamic::Create(Base, this);
        if (!MeshMaterial)
        {
            return;
        }
    }

    // Re-assert the override every time rather than only on creation: swapping the
    // static mesh for a `shape` change can drop the component's material overrides.
    if (Component->GetMaterial(0) != MeshMaterial.Get())
    {
        Component->SetMaterial(0, MeshMaterial);
    }

    MeshMaterial->SetVectorParameterValue(ColorParameter, Spec.Color);
    MeshMaterial->SetScalarParameterValue(RoughnessParameter, Spec.Roughness);
}

void ALoomaSyncedActor::ApplyLight(const FLoomaNodeComponents& Next, const FLoomaNodeRenderContext& Context)
{
    if (!Next.bHasLight)
    {
        DestroySlot(LightComponent);
        return;
    }

    const FLoomaLightSpec& Spec = Next.Light;
    UClass* Wanted = LightClassFor(Spec.LightType);
    if (LightComponent && LightComponent->GetClass() != Wanted)
    {
        DestroySlot(LightComponent); // `lightType` changed — that is a different UE class
    }
    if (!LightComponent)
    {
        // NAME_None, not a fixed name: a component edit can remove and re-add a light,
        // and a destroyed component still owns its name until the next GC.
        ULightComponent* Created = NewObject<ULightComponent>(this, Wanted, NAME_None);
        if (!Created)
        {
            return;
        }
        Created->SetMobility(EComponentMobility::Movable);
        Created->SetupAttachment(SceneRoot);
        Created->RegisterComponent();
        AddInstanceComponent(Created);
        LightComponent = Created;
    }

    ULightComponent* Light = LightComponent.Get();

    // UE stores LightColor as sRGB and de-gammas it when computing radiance, so hand
    // SetLightColor the sRGB form of our linear colour (bSRGB = true re-applies the
    // gamma LoomaParseColor removed). three.js does the same de-gamma, so both
    // clients light with the same energy.
    Light->SetLightColor(Spec.Color, /*bSRGB=*/true);
    Light->SetCastShadows(Spec.bCastShadow);

    // ---- Intensity: three.js physical units → UE physical units ----------------
    //
    // The web client renders point and spot lights with `decay = 2`, which puts
    // three.js in its physically-correct mode: **point/spot intensity is candela**
    // and **directional intensity is lux**. UE measures a local light in whichever
    // ELightUnits it is told, and a directional light in lux. So the conversion is
    // 1:1 in matching units — Candelas for point/spot, the bare number for
    // directional — and no magic constant is needed.
    //
    // What still differs is *exposure*, not the light: UE's auto-exposure and tone
    // mapper are not three.js's, so the same candela value can read brighter or
    // darker on screen. `LightIntensityScale` (config, default 1.0) is the trim for
    // that, and it is the only place a fudge factor belongs.
    const float Intensity = FMath::Max(0.0f, Spec.Intensity) * Context.LightIntensityScale;
    if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light))
    {
        Local->SetIntensityUnits(ELightUnits::Candelas);
        Local->SetIntensity(Intensity);
        Local->SetAttenuationRadius(Spec.Distance > 0.0f
            ? static_cast<float>(Spec.Distance * WireToCm)
            : UnlimitedAttenuationCm);
    }
    else
    {
        Light->SetIntensity(Intensity); // directional — lux on both sides
    }

    if (USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
    {
        // Both engines take a half-angle; only the unit differs (radians vs degrees).
        const float Outer = FMath::RadiansToDegrees(FMath::Clamp(Spec.Angle, 0.0f, UE_HALF_PI));
        Spot->SetOuterConeAngle(Outer);
        // three.js `penumbra` is the fraction of the cone that is soft edge: 0 is a
        // hard edge (inner == outer), 1 is soft all the way in (inner == 0).
        Spot->SetInnerConeAngle(Outer * (1.0f - FMath::Clamp(Spec.Penumbra, 0.0f, 1.0f)));
    }

    // Aim: **the node's local −Y is the beam direction** — the convention the web
    // client sets by parking a spot/directional light's target at local (0, −1, 0).
    // Wire −Y maps to UE −Z, and a UE light shines down its component's +X, so a
    // pitch of −90° turns +X into −Z. A point light is radial and needs no aim.
    const bool bAimed = Spec.LightType != ELoomaLightType::Point;
    Light->SetRelativeRotation(bAimed ? FRotator(-90.0f, 0.0f, 0.0f) : FRotator::ZeroRotator);
}

UStaticMeshComponent* ALoomaSyncedActor::EnsureMeshComponent(TObjectPtr<UStaticMeshComponent>& Slot)
{
    if (UStaticMeshComponent* Existing = Slot.Get())
    {
        return Existing;
    }
    UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this);
    if (!Component)
    {
        return nullptr;
    }
    Component->SetMobility(EComponentMobility::Movable);
    Component->SetupAttachment(SceneRoot);
    Component->RegisterComponent();
    AddInstanceComponent(Component); // so it shows in the Details panel while running
    Slot = Component;
    return Component;
}

// --- The `model` component's GLB ----------------------------------------------

void ALoomaSyncedActor::LoadMeshFromUrl(const FString& Url)
{
    if (!EnsureMeshComponent(ModelComponent))
    {
        return;
    }
    // Default LoaderConfig: glTFRuntime's default SceneBasis/SceneScale is the same
    // Y-up-meters -> Z-up-centimeters conversion the sync layer uses for transforms,
    // so geometry and poses stay consistent. Do not override it.
    FglTFRuntimeConfig LoaderConfig;
    FglTFRuntimeHttpResponse Completed;
    Completed.BindDynamic(this, &ALoomaSyncedActor::OnGlbLoaded);
    // The empty header map is deliberate, and this is the obvious place to be tempted
    // to fill it with the session bearer. Do not: `/static` is a plain StaticFiles
    // mount (backend/app/main.py) and the FastAPI app carries no app-wide
    // `dependencies=` and no auth middleware — only individual routes have
    // `require_admin` — so the GLB fetch is anonymous by design and a bearer here would
    // be a token sent somewhere it is not read. glTFRuntime does take headers (this
    // parameter), so if `/static` is ever gated the seam is right here; it would want
    // ULoomaSceneSyncSubsystem::ApplyAuthHeader's map overload rather than a second
    // spelling of the header.
    UglTFRuntimeFunctionLibrary::glTFLoadAssetFromUrl(Url, TMap<FString, FString>(), Completed, LoaderConfig);
}

void ALoomaSyncedActor::OnGlbLoaded(UglTFRuntimeAsset* Asset)
{
    UStaticMeshComponent* Component = ModelComponent.Get();
    if (!IsValid(this) || !Component || !Asset)
    {
        UE_LOG(LogLoomaSync, Warning, TEXT("Failed to load GLB for asset '%s'"), *AssetId);
        return;
    }
    // A `model` is an opaque whole sub-scene — a GLB is a node hierarchy with its own
    // meshes and materials, and every client treats it as one unit. Merging the whole
    // node tree into a single static mesh is exactly that.
    FglTFRuntimeStaticMeshConfig MeshConfig;
    UStaticMesh* Mesh = Asset->LoadStaticMeshRecursive(TEXT(""), TArray<FString>(), MeshConfig);
    if (!Mesh)
    {
        UE_LOG(LogLoomaSync, Warning, TEXT("GLB for '%s' produced no static mesh"), *AssetId);
        return;
    }
    Component->SetStaticMesh(Mesh);

    // GLBs are placed by their base, not their middle: the catalog normalises them to
    // a unit box centred on the origin, while every placement path hands out a point
    // picked off a surface, so a centre-pivot model would be buried to the waist. The
    // web client lifts the model inside its own group (pivot.js); this is the same
    // offset, one level below the node's transform, so the node's own pose — what the
    // gizmo drives and what goes on the wire — is untouched.
    const FBox Box = Mesh->GetBoundingBox();
    Component->SetRelativeLocation(bBaseAlignModel ? FVector(0.0, 0.0, -Box.Min.Z) : FVector::ZeroVector);

    // Datalake assets are normalized to a ~1 m (100 cm) bounding box; log the built
    // size so a scale/basis regression is visible in the log.
    UE_LOG(LogLoomaSync, Log, TEXT("Mesh for '%s' built, bbox size %s (cm), center %s"),
        *AssetId, *Box.GetSize().ToString(), *Box.GetCenter().ToString());
}

// --- Pose ---------------------------------------------------------------------

FTransform ALoomaSyncedActor::GetLocalTransform() const
{
    // Parent-local, because that is what the wire carries. For an unattached actor
    // this is its world pose, so a root needs no special case.
    const USceneComponent* Root = GetRootComponent();
    return Root ? Root->GetRelativeTransform() : FTransform::Identity;
}

void ALoomaSyncedActor::SetLocalTransform(const FTransform& Local)
{
    if (USceneComponent* Root = GetRootComponent())
    {
        Root->SetRelativeTransform(Local);
    }
}

void ALoomaSyncedActor::SetRemoteTarget(const FTransform& LocalTarget, bool bSnap)
{
    if (bSnap)
    {
        bHasRemoteTarget = false;
        SetLocalTransform(LocalTarget);
        return;
    }
    RemoteTarget = LocalTarget;
    bHasRemoteTarget = true;
}

void ALoomaSyncedActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bHasRemoteTarget)
    {
        return;
    }

    // Exponential ease toward the target — mirrors the web RemoteSmoother. Each node
    // smooths its own *local* pose, so a child eases inside its parent rather than
    // fighting it.
    const float Alpha = 1.0f - FMath::Exp(-12.0f * DeltaSeconds);
    const FTransform Current = GetLocalTransform();
    FTransform Next;
    Next.SetLocation(FMath::Lerp(Current.GetLocation(), RemoteTarget.GetLocation(), Alpha));
    Next.SetRotation(FQuat::Slerp(Current.GetRotation(), RemoteTarget.GetRotation(), Alpha));
    Next.SetScale3D(FMath::Lerp(Current.GetScale3D(), RemoteTarget.GetScale3D(), Alpha));
    SetLocalTransform(Next);

    const bool bClose =
        FVector::DistSquared(Next.GetLocation(), RemoteTarget.GetLocation()) < 0.01 && // (0.1 cm)^2
        Next.GetRotation().AngularDistance(RemoteTarget.GetRotation()) < 0.001 &&
        FVector::DistSquared(Next.GetScale3D(), RemoteTarget.GetScale3D()) < 1e-8;
    if (bClose)
    {
        SetLocalTransform(RemoteTarget);
        bHasRemoteTarget = false;
    }
}
