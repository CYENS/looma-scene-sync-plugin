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
 * (looma-xr-asset-demo/frontend/src/scene/components/{mesh,light,audio}.jsx), because a
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

/** How an `audio` component fades with distance. See FLoomaAudioSpec. */
UENUM(BlueprintType)
enum class ELoomaAudioRolloff : uint8
{
    /** gain = minDistance / d: how sound really falls off. Unity's Logarithmic. */
    Inverse,
    /** gain = 1 - (d - minDistance) / (maxDistance - minDistance), silent past maxDistance. */
    Linear,
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

/**
 * `animation`: which clip embedded in the sibling `model`'s GLB the node plays.
 *
 * Wire shape `{"type": "animation", "clip": "Walk" | null, "playing": true | false}`.
 * Clip names are the glTF animation names a rigged GLB carries (`Idle`, `Walk`,
 * `Wave`, `Dance` — looma-xr-asset-demo/docs/rig-format.md), matched exactly. It
 * only means something beside a `model` whose GLB is skinned; on anything else it is
 * carried and ignored, the same way a `material` beside a `model` is.
 */
USTRUCT(BlueprintType)
struct FLoomaAnimationSpec
{
    GENERATED_BODY()

    /** Clip name; empty for a wire `null`, which means "no clip — stand at rest". */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString Clip;

    /**
     * False stops the clip and stands the character at rest. Absent reads as true:
     * naming a clip is asking for it to play.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bPlaying = true;

    /** The clip that should be on screen right now; empty means the rest pose. */
    FString ActiveClip() const { return bPlaying ? Clip : FString(); }

    bool operator==(const FLoomaAnimationSpec& Other) const
    {
        return Clip == Other.Clip && bPlaying == Other.bPlaying;
    }
    bool operator!=(const FLoomaAnimationSpec& Other) const { return !(*this == Other); }
};

/**
 * `audio`: a sound source. On its own node it is a speaker; beside a `model` it is
 * that model's own sound (looma-xr-asset-demo/docs/scene-format.md, "The `audio`
 * component").
 *
 * The fields are the ones Unity's AudioSource, UAudioComponent and Web Audio's
 * PannerNode share, under Unity's names. Two defaults differ from both engines on
 * purpose: `loop` and `spatial` are true, because a sound placed in a shared scene is
 * a thing in the room, heard where it is for as long as the scene is open.
 */
USTRUCT(BlueprintType)
struct FLoomaAudioSpec
{
    GENERATED_BODY()

    /** Datalake sound. The WAV url is rebuilt from this, as a model's GLB url is. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString AssetId;

    /** Whether it sounds. Turning true starts it from the beginning. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bPlaying = true;

    /** Start again at the end. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bLoop = true;

    /** Gain, 0…1: UAudioComponent's VolumeMultiplier. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Volume = 1.0f;

    /** Playback rate, 0.5…2, which changes the speed too: PitchMultiplier. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Pitch = 1.0f;

    /** Heard from the node and fading with distance; false is heard at Volume everywhere. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bSpatial = true;

    /** Wire metres within which it is at full volume: the attenuation sphere's radius. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float MinDistance = 1.0f;

    /** Wire metres at which the fade stops: the sphere's radius plus its falloff. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float MaxDistance = 20.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaAudioRolloff Rolloff = ELoomaAudioRolloff::Inverse;

    bool operator==(const FLoomaAudioSpec& Other) const
    {
        return AssetId == Other.AssetId && bPlaying == Other.bPlaying && bLoop == Other.bLoop
            && FMath::IsNearlyEqual(Volume, Other.Volume) && FMath::IsNearlyEqual(Pitch, Other.Pitch)
            && bSpatial == Other.bSpatial && FMath::IsNearlyEqual(MinDistance, Other.MinDistance)
            && FMath::IsNearlyEqual(MaxDistance, Other.MaxDistance) && Rolloff == Other.Rolloff;
    }
    bool operator!=(const FLoomaAudioSpec& Other) const { return !(*this == Other); }
};

/** Every component on one node, keyed by type. */
/** The `source` of a `livelink` component the backend's own Live Link drives (HAM-328, HAM-329). */
inline constexpr const TCHAR* LoomaLiveLinkHubSource = TEXT("hub");

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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasAnimation = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaAnimationSpec Animation;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasAudio = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaAudioSpec Audio;

    /**
     * The `livelink` component's subject: the Live Link subject that drives this
     * node, or empty for nobody (HAM-316; docs/rig-format.md, "Who drives a
     * character", in looma-xr-asset-demo). This plugin only carries it. Whoever
     * has Live Link reads it and poses the character.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString LiveLinkSubject;

    /**
     * Who has LiveLinkSubject (HAM-329): empty for a client, as before, or
     * LoomaLiveLinkHubSource for the backend's own Live Link, which then drives
     * the node itself and drops any client's poses for it. A client must not
     * drive a hub node from its own Live Link even if it has a subject of that
     * name, because that is another performer; it shows the hub's poses instead.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString LiveLinkSource;

    /**
     * Which of the hub's streams LiveLinkSubject is in, when the hub has it: one
     * mocap service, named in the backend's /admin (HAM-328). Empty means
     * whichever of the hub's streams has a subject of that name.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString LiveLinkStream;

    /** Whether the backend's hub, not a client, drives this node. */
    bool IsLiveLinkFromHub() const { return !LiveLinkSubject.IsEmpty() && LiveLinkSource == LoomaLiveLinkHubSource; }

    /** Types this build does not understand. Logged once each; never rendered, never dropped. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TArray<FString> UnknownTypes;

    /** True for an empty object — a transform and nothing else. */
    bool IsEmpty() const { return !bHasModel && !bHasMesh && !bHasLight && !bHasAudio; }

    bool operator==(const FLoomaNodeComponents& Other) const
    {
        return bHasModel == Other.bHasModel && (!bHasModel || Model == Other.Model)
            && bHasMesh == Other.bHasMesh && (!bHasMesh || Mesh == Other.Mesh)
            && bHasMaterial == Other.bHasMaterial && (!bHasMaterial || Material == Other.Material)
            && bHasLight == Other.bHasLight && (!bHasLight || Light == Other.Light)
            && bHasAnimation == Other.bHasAnimation && (!bHasAnimation || Animation == Other.Animation)
            && bHasAudio == Other.bHasAudio && (!bHasAudio || Audio == Other.Audio)
            && LiveLinkSubject == Other.LiveLinkSubject && LiveLinkSource == Other.LiveLinkSource
            && LiveLinkStream == Other.LiveLinkStream;
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

/**
 * A node's components with its `livelink` component set to `Subject`, or removed
 * when `Subject` is empty (HAM-316). Everything else is carried over as it came,
 * including components this client does not understand: a component edit is the
 * node's WHOLE array, so whatever is left out of it is deleted for everybody.
 *
 * `Source` and `Stream` (HAM-329) say who has the subject: empty for a client,
 * which is what the component gets when they are left out, or the hub
 * (LoomaLiveLinkHubSource) and one of its streams. The component is written
 * whole, so choosing a client's subject takes a previous source and stream off.
 */
LOOMASCENESYNC_API TArray<TSharedPtr<FJsonValue>> LoomaWithLiveLinkSubject(
    const TArray<TSharedPtr<FJsonValue>>& Components, const FString& Subject,
    const FString& Source = FString(), const FString& Stream = FString());
