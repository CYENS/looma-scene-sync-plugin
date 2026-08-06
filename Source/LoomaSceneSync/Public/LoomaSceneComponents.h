#pragma once

#include "CoreMinimal.h"
#include "LoomaSceneComponents.generated.h"

class FJsonObject;
class FJsonValue;

/**
 * A node's components, parsed off the wire (scene format v3 — the normative
 * contract is looma-xr-asset-demo/docs/scene-format.md).
 *
 * A node is an object with a transform; what it *is* comes from the components
 * attached to it. This header is the seam between the wire document and Unreal:
 * `ALoomaSyncedActor::ApplyComponents` maps these specs onto engine components and
 * knows nothing about JSON.
 *
 * Two format rules shape the types below:
 *
 *  - **One component per type per node**, so this is a keyed struct with a
 *    `bHas*` flag each, not a list. Two meshes means two child nodes.
 *  - **An unknown `type` is skipped while the node is kept.** Unknown types are
 *    collected in `UnknownTypes` for logging only; nothing else in the plugin
 *    branches on them. That rule is what lets the backend and the web client ship
 *    a new component type without a coordinated Unreal release.
 *
 * Defaults here are deliberately the *web client's* defaults
 * (looma-xr-asset-demo/frontend/src/scene/components/{mesh,light}.jsx), because a
 * field absent from the payload is filled in by whoever renders it — the stored
 * document never carries defaults.
 */

UENUM(BlueprintType)
enum class ELoomaMeshShape : uint8
{
    Box,
    Sphere,
    Plane,
    Cylinder,
};

UENUM(BlueprintType)
enum class ELoomaLightType : uint8
{
    Point,
    Spot,
    Directional,
};

/** `model`: a datalake GLB — an opaque whole sub-scene, not one mesh. */
USTRUCT(BlueprintType)
struct FLoomaModelSpec
{
    GENERATED_BODY()

    /** Datalake asset id. The GLB url is rebuilt from this; the wire's `url` is a web-only hint. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString AssetId;

    /**
     * Generation job that produced this instance, if any.
     *
     * Carried *inside* the model component rather than on the node, because the hub
     * normalises every node down to id/parent/name/t/components
     * (`scenegraph._sanitize`) and would drop a node-level `jobId`. Extra keys on a
     * known component type are preserved, so this survives the round trip.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString JobId;

    bool operator==(const FLoomaModelSpec& Other) const
    {
        return AssetId == Other.AssetId && JobId == Other.JobId;
    }
    bool operator!=(const FLoomaModelSpec& Other) const { return !(*this == Other); }
};

/** `mesh`: procedural geometry. Appearance lives in a sibling `material`. */
USTRUCT(BlueprintType)
struct FLoomaMeshSpec
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaMeshShape Shape = ELoomaMeshShape::Box;

    /**
     * Extent in **wire** metres and **wire** axes (right-handed, Y-up), exactly as
     * authored. Converted to UE axes and centimetres at apply time — which shape
     * reads which component differs, see LoomaSyncedActor.cpp.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FVector Size = FVector::OneVector;

    bool operator==(const FLoomaMeshSpec& Other) const
    {
        return Shape == Other.Shape && Size.Equals(Other.Size, 1e-6);
    }
    bool operator!=(const FLoomaMeshSpec& Other) const { return !(*this == Other); }
};

/** `material`: the appearance of a sibling `mesh`. A `model` carries its own. */
USTRUCT(BlueprintType)
struct FLoomaMaterialSpec
{
    GENERATED_BODY()

    /** Linear colour, already converted out of the wire's sRGB hex. ≈ the web's #cccccc. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLinearColor Color = FLinearColor(0.604f, 0.604f, 0.604f);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Roughness = 0.6f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Metalness = 0.0f;

    bool operator==(const FLoomaMaterialSpec& Other) const
    {
        return Color.Equals(Other.Color, 1e-6f)
            && FMath::IsNearlyEqual(Roughness, Other.Roughness)
            && FMath::IsNearlyEqual(Metalness, Other.Metalness);
    }
    bool operator!=(const FLoomaMaterialSpec& Other) const { return !(*this == Other); }
};

/** `light`: make any object a lantern. */
USTRUCT(BlueprintType)
struct FLoomaLightSpec
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaLightType LightType = ELoomaLightType::Point;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLinearColor Color = FLinearColor::White;

    /** three.js units: candela for point/spot, lux for directional. See LoomaSyncedActor.cpp. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Intensity = 2.0f;

    /** Metres; 0 means "no falloff limit" in three.js. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Distance = 0.0f;

    /** Spot only. Radians, half-angle — the same convention as UE's cone angles in degrees. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Angle = 0.6f;

    /** Spot only. 0…1 — the fraction of the cone that is soft edge. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Penumbra = 0.9f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bCastShadow = false;

    bool operator==(const FLoomaLightSpec& Other) const
    {
        return LightType == Other.LightType
            && Color.Equals(Other.Color, 1e-6f)
            && FMath::IsNearlyEqual(Intensity, Other.Intensity)
            && FMath::IsNearlyEqual(Distance, Other.Distance)
            && FMath::IsNearlyEqual(Angle, Other.Angle)
            && FMath::IsNearlyEqual(Penumbra, Other.Penumbra)
            && bCastShadow == Other.bCastShadow;
    }
    bool operator!=(const FLoomaLightSpec& Other) const { return !(*this == Other); }
};

/** Every component on one node, keyed by type. */
USTRUCT(BlueprintType)
struct FLoomaNodeComponents
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasModel = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaModelSpec Model;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasMesh = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaMeshSpec Mesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasMaterial = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaMaterialSpec Material;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasLight = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaLightSpec Light;

    /** Types this build does not understand. Logged once each; never rendered, never dropped. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TArray<FString> UnknownTypes;

    /** True for an empty object — a transform and nothing else. */
    bool IsEmpty() const { return !bHasModel && !bHasMesh && !bHasLight; }

    bool operator==(const FLoomaNodeComponents& Other) const
    {
        return bHasModel == Other.bHasModel && (!bHasModel || Model == Other.Model)
            && bHasMesh == Other.bHasMesh && (!bHasMesh || Mesh == Other.Mesh)
            && bHasMaterial == Other.bHasMaterial && (!bHasMaterial || Material == Other.Material)
            && bHasLight == Other.bHasLight && (!bHasLight || Light == Other.Light);
    }
    bool operator!=(const FLoomaNodeComponents& Other) const { return !(*this == Other); }
};

/**
 * Parse a node's `components` array. A null/absent array is an empty object, which
 * is a perfectly good node — its children's transforms are relative to it.
 */
LOOMASCENESYNC_API FLoomaNodeComponents LoomaParseComponents(const TArray<TSharedPtr<FJsonValue>>* Components);

/**
 * `#rgb` / `#rrggbb` (sRGB, as authored in the browser) → linear colour.
 * Anything else returns `Fallback` — the wire is repaired, never rejected.
 */
LOOMASCENESYNC_API FLinearColor LoomaParseColor(const FString& Hex, const FLinearColor& Fallback);

/**
 * Build the `model` component a spawn originating in Unreal puts on the wire.
 *
 * `WebUrl` is emitted even though this client *ignores* `url` on the way in, and the
 * asymmetry is load-bearing: a web client loads the GLB from `url` and renders
 * nothing at all without it (`model.jsx` returns null), while the hub never
 * synthesises one — a field absent from the payload stays absent, or every read of a
 * scene would differ from what is stored and rewrite the row. So a spawn from Unreal
 * carrying only `assetId` arrives in the browser as an object with no geometry.
 */
LOOMASCENESYNC_API TSharedRef<FJsonObject> LoomaMakeModelComponent(const FString& AssetId, const FString& JobId,
    const FString& WebUrl);
