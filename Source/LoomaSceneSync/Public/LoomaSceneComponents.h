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

/** Portable panorama and optional HDR lighting. Right-handed Y-up degrees. */
USTRUCT(BlueprintType)
struct FLoomaSkyboxSpec
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Looma")
    FString AssetId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Looma")
    FString Url;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Looma")
    float Rotation = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Looma")
    float Exposure = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Looma")
    FLinearColor Tint = FLinearColor::White;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Looma")
    bool bLighting = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Looma", meta = (ClampMin = "0", ClampMax = "16"))
    float LightingIntensity = 1.0f;

    bool operator==(const FLoomaSkyboxSpec& Other) const
    {
        return AssetId == Other.AssetId && Url == Other.Url && Tint.Equals(Other.Tint)
            && FMath::IsNearlyEqual(Rotation, Other.Rotation) && FMath::IsNearlyEqual(Exposure, Other.Exposure)
            && bLighting == Other.bLighting && FMath::IsNearlyEqual(LightingIntensity, Other.LightingIntensity);
    }
};

/** `camera`'s `projection`. */
UENUM(BlueprintType)
enum class ELoomaCameraProjection : uint8
{
    Perspective,
    Orthographic,
};

/**
 * `camera`: a viewpoint. The node looks down its local −Z with +Y up, glTF's and
 * three.js's camera axes, which is UE +X with +Z up after LoomaWireConvert's map, so
 * the camera component needs no turn of its own (looma-xr-asset-demo/docs/
 * scene-format.md, "The `camera` component"). Defaults and clamps are the web's
 * (frontend/src/scene/camera/lens.js), which are the hub's (`_clean_camera`).
 */
USTRUCT(BlueprintType)
struct FLoomaCameraSpec
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaCameraProjection Projection = ELoomaCameraProjection::Perspective;

    /** **Vertical** field of view, degrees, 1…170. Unreal's own FieldOfView is horizontal. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Fov = 50.0f;

    /** Half the view's height, wire metres (orthographic only). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float OrthoSize = 5.0f;

    /** Near clip plane, wire metres, at least 0.001. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Near = 0.1f;

    /**
     * Far clip plane, wire metres, beyond Near. Only an orthographic view uses it:
     * Unreal's perspective far plane is always at infinity.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Far = 1000.0f;

    /** Whether the camera has a frame of its own. Without one it fills whatever screen it is on. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasAspect = false;

    /** Width over height of that frame, 0.1…10; shown letterboxed. Meaningless without bHasAspect. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Aspect = 16.0f / 9.0f;

    /** Claims to be the scene's camera. The first in document order that is switched on wins. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bPrimary = false;

    bool operator==(const FLoomaCameraSpec& Other) const
    {
        return Projection == Other.Projection && FMath::IsNearlyEqual(Fov, Other.Fov)
            && FMath::IsNearlyEqual(OrthoSize, Other.OrthoSize) && FMath::IsNearlyEqual(Near, Other.Near)
            && FMath::IsNearlyEqual(Far, Other.Far) && bHasAspect == Other.bHasAspect
            && (!bHasAspect || FMath::IsNearlyEqual(Aspect, Other.Aspect)) && bPrimary == Other.bPrimary;
    }
    bool operator!=(const FLoomaCameraSpec& Other) const { return !(*this == Other); }
};

/** `path`'s `mode`: what the carriage does at the end of the curve. */
UENUM(BlueprintType)
enum class ELoomaPathMode : uint8
{
    Loop,
    PingPong,
    Once,
};

/** `path`'s `ease`. */
UENUM(BlueprintType)
enum class ELoomaPathEase : uint8
{
    Linear,
    /** Slow out of each end and into the other: u²(3 − 2u). */
    InOut,
};

/** `path`'s `orient`: whether the carriage turns along the curve. */
UENUM(BlueprintType)
enum class ELoomaPathOrient : uint8
{
    /** The carriage keeps the node's own axes. */
    Fixed,
    /** The carriage's −Z points along the curve, +Y as near the node's +Y as it can be. */
    Path,
};

/**
 * `path`: a curve the node's **children** ride along, on a carriage, on the hub's
 * clock (scene-format.md, "The `path` component"; the maths is LoomaDrivers.h).
 */
USTRUCT(BlueprintType)
struct FLoomaPathSpec
{
    GENERATED_BODY()

    /** The points the curve passes through, in the node's own frame: wire metres and wire axes. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TArray<FVector> Points;

    /** Join the last point back to the first. Needs three points; two are drawn open. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bClosed = false;

    /** Seconds for one traversal, 0.1…86400. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Duration = 10.0f;

    /** False holds the carriage at Offset. Only a literal false pauses. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bPlaying = true;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaPathMode Mode = ELoomaPathMode::Loop;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaPathEase Ease = ELoomaPathEase::Linear;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaPathOrient Orient = ELoomaPathOrient::Fixed;

    /** Whether the hub has stamped StartedAt yet. A path it has not waits at Offset. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bStamped = false;

    /** The hub's wall clock, Unix seconds, when the carriage was last anchored. The hub's alone. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    double StartedAt = 0.0;

    /** Seconds of travel at StartedAt. The hub's alone. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    double Offset = 0.0;

    bool operator==(const FLoomaPathSpec& Other) const
    {
        if (Points.Num() != Other.Points.Num())
        {
            return false;
        }
        for (int32 I = 0; I < Points.Num(); ++I)
        {
            if (!Points[I].Equals(Other.Points[I], 1e-9))
            {
                return false;
            }
        }
        return bClosed == Other.bClosed && FMath::IsNearlyEqual(Duration, Other.Duration)
            && bPlaying == Other.bPlaying && Mode == Other.Mode && Ease == Other.Ease && Orient == Other.Orient
            && bStamped == Other.bStamped && StartedAt == Other.StartedAt && Offset == Other.Offset;
    }
    bool operator!=(const FLoomaPathSpec& Other) const { return !(*this == Other); }
};

/** `lookAt`'s `axis`: which local axis is turned to point at the target. */
UENUM(BlueprintType)
enum class ELoomaLookAtAxis : uint8
{
    /** A camera's view and glTF's forward. */
    MinusZ,
    /** The way a `light` shines in this format. */
    MinusY,
};

/**
 * `lookAt`: turn the node, every frame, so one of its axes points at another node.
 * The turn is drawn on the actor's AimComponent; the node's stored `t` never moves
 * (scene-format.md, "The `lookAt` component").
 */
USTRUCT(BlueprintType)
struct FLoomaLookAtSpec
{
    GENERATED_BODY()

    /** The node to face. Empty (a wire `null`) aims nothing, as does an id the scene does not hold. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString Target;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaLookAtAxis Axis = ELoomaLookAtAxis::MinusZ;

    /** The point aimed at, in the target's own frame: wire metres and wire axes. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FVector Offset = FVector::ZeroVector;

    bool operator==(const FLoomaLookAtSpec& Other) const
    {
        return Target == Other.Target && Axis == Other.Axis && Offset.Equals(Other.Offset, 1e-9);
    }
    bool operator!=(const FLoomaLookAtSpec& Other) const { return !(*this == Other); }
};

/** `postProcess`'s `toneMapping`. */
UENUM(BlueprintType)
enum class ELoomaToneMapping : uint8
{
    None,
    Aces,
    Agx,
    /** Khronos PBR Neutral. */
    Neutral,
};

/**
 * `postProcess`: how the picture looks, through one camera or everywhere
 * (scene-format.md, "The `postProcess` component").
 *
 * **Every setting is optional, and an absent one overrides nothing**: each carries a
 * `bHas…` flag, Unity's per-setting override switch and Unreal's `bOverride_…`. The
 * numbers are in the format's units; LoomaPostProcess.h maps them onto
 * FPostProcessSettings.
 */
USTRUCT(BlueprintType)
struct FLoomaPostProcessSpec
{
    GENERATED_BODY()

    /** Order among global volumes: higher is applied later and wins. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Priority = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasExposure = false;
    /** EV stops, −10…10. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Exposure = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasToneMapping = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    ELoomaToneMapping ToneMapping = ELoomaToneMapping::Aces;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasContrast = false;
    /** −1…1, about mid grey. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Contrast = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasSaturation = false;
    /** −1…1. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Saturation = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasColorFilter = false;
    /** Linear, already converted out of the wire's sRGB hex. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLinearColor ColorFilter = FLinearColor::White;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasBloomIntensity = false;
    /** 0…10; 0 is off. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float BloomIntensity = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasBloomThreshold = false;
    /** Linear luminance, 0…100: 1 is brighter than white. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float BloomThreshold = 1.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasVignette = false;
    /** 0…1. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Vignette = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasDepthOfField = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bDepthOfField = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasFocusDistance = false;
    /** Wire metres, 0.01…10000. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float FocusDistance = 5.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasFStop = false;
    /** 0.5…64. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float FStop = 2.8f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasChromaticAberration = false;
    /** 0…1. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float ChromaticAberration = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasGrain = false;
    /** 0…1. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    float Grain = 0.0f;

    /** True when it carries no setting at all, and so changes nothing. Priority is not a setting. */
    bool IsEmpty() const
    {
        return !bHasExposure && !bHasToneMapping && !bHasContrast && !bHasSaturation && !bHasColorFilter
            && !bHasBloomIntensity && !bHasBloomThreshold && !bHasVignette && !bHasDepthOfField
            && !bHasFocusDistance && !bHasFStop && !bHasChromaticAberration && !bHasGrain;
    }

    /** Lay `Above` over this: every setting it carries replaces this one's, the rest stay. */
    LOOMASCENESYNC_API void Overlay(const FLoomaPostProcessSpec& Above);

    LOOMASCENESYNC_API bool operator==(const FLoomaPostProcessSpec& Other) const;
    bool operator!=(const FLoomaPostProcessSpec& Other) const { return !(*this == Other); }
};

/** Every component on one node, keyed by type. */
/** The `source` of a `livelink` component the backend's own Live Link drives (HAM-328, HAM-329). */
inline constexpr const TCHAR* LoomaLiveLinkHubSource = TEXT("hub");

USTRUCT(BlueprintType)
struct FLoomaNodeComponents
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasSkybox = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaSkyboxSpec Skybox;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHidden = false;

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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasCamera = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaCameraSpec Camera;

    /**
     * A `camera` is on the node, switched on or off. Not the same as bHasCamera: it
     * decides whether the node's `postProcess` is that camera's or a global one, and a
     * camera switched off still makes it the camera's (frontend/src/scene/postprocess/
     * look.js, `resolveLook`), which then applies nowhere.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bCarriesCamera = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasPath = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaPathSpec Path;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasLookAt = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaLookAtSpec LookAt;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    bool bHasPostProcess = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FLoomaPostProcessSpec PostProcess;

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
    bool IsEmpty() const
    {
        return !bHasModel && !bHasMesh && !bHasLight && !bHasAudio && !bHasSkybox && !bHasCamera && !bHasPath
            && !bHasLookAt && !bHasPostProcess;
    }

    bool operator==(const FLoomaNodeComponents& Other) const
    {
        return bHasModel == Other.bHasModel && (!bHasModel || Model == Other.Model)
            && bHasSkybox == Other.bHasSkybox && (!bHasSkybox || Skybox == Other.Skybox) && bHidden == Other.bHidden
            && bHasMesh == Other.bHasMesh && (!bHasMesh || Mesh == Other.Mesh)
            && bHasMaterial == Other.bHasMaterial && (!bHasMaterial || Material == Other.Material)
            && bHasLight == Other.bHasLight && (!bHasLight || Light == Other.Light)
            && bHasAnimation == Other.bHasAnimation && (!bHasAnimation || Animation == Other.Animation)
            && bHasAudio == Other.bHasAudio && (!bHasAudio || Audio == Other.Audio)
            && bHasCamera == Other.bHasCamera && (!bHasCamera || Camera == Other.Camera)
            && bCarriesCamera == Other.bCarriesCamera
            && bHasPath == Other.bHasPath && (!bHasPath || Path == Other.Path)
            && bHasLookAt == Other.bHasLookAt && (!bHasLookAt || LookAt == Other.LookAt)
            && bHasPostProcess == Other.bHasPostProcess && (!bHasPostProcess || PostProcess == Other.PostProcess)
            && LiveLinkSubject == Other.LiveLinkSubject && LiveLinkSource == Other.LiveLinkSource
            && LiveLinkStream == Other.LiveLinkStream;
    }
    bool operator!=(const FLoomaNodeComponents& Other) const { return !(*this == Other); }
};

/**
 * Parse the active runtime specs from a node's `components` array. A literal
 * `enabled: false` omits that spec; the actor retains the full wire array for edits.
 * A null/absent array is an empty object, which is a perfectly good node — its
 * children's transforms are relative to it.
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
