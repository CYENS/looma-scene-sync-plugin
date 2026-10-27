#pragma once

#include "CoreMinimal.h"
#include "Engine/Scene.h"
#include "LoomaSceneComponents.h"

/**
 * The `postProcess` component as Unreal post-processing (looma-xr-asset-demo/docs/
 * scene-format.md, "The `postProcess` component"). Pure, so the precedence and the
 * field mapping can be tested without a renderer.
 *
 * The model is Unity's Volume and Unreal's post-process volume, without spatial
 * blending, and the same as the web client's (frontend/src/scene/postprocess/look.js):
 *
 *  - On a node with no `camera`, a volume is GLOBAL: every view of the scene.
 *  - On a camera's node, it applies while somebody looks through that camera, on top
 *    of every global one: the camera component's own post-process settings.
 *  - Globals stack by `priority`, lowest first, then in document order. Each overrides
 *    only the settings it carries, so the stack is resolved here into one look, and
 *    one look is what Unreal is given.
 *  - A volume on a hidden object, or under one, or switched off itself, applies nothing.
 */

/** One global volume, with its place in the document. */
struct FLoomaLookVolume
{
    FLoomaPostProcessSpec Spec;
    /** Document order, which breaks a tie in priority. */
    int32 Order = 0;
};

/**
 * The look that applies to a view: the globals by priority then order, then a
 * camera's own on top when `CameraOwn` is given. False when no volume applies at all,
 * which must leave the picture exactly as it was.
 */
LOOMASCENESYNC_API bool LoomaResolveLook(TArray<FLoomaLookVolume> Globals, const FLoomaPostProcessSpec* CameraOwn,
    FLoomaPostProcessSpec& OutLook);

/**
 * FPostProcessSettings with an override switched on for every setting `Look` carries
 * and for nothing else. `FrameAspect` is the frame's width over height, which depth
 * of field needs to put the lens on a full-frame gate.
 *
 *   exposure            AutoExposureBias, metering AEM_Manual, physical camera exposure off
 *   toneMapping         ToneCurveAmount: 0 for `none`, 1 for the others (Unreal's filmic
 *                       curve is always on, so `aces`, `agx` and `neutral` all keep it)
 *   contrast            ColorContrast = 1 + contrast on every channel
 *   saturation          ColorSaturation = 1 + saturation
 *   colorFilter         ColorGain
 *   bloomIntensity      BloomIntensity (≈: scales differ between engines)
 *   bloomThreshold      BloomThreshold
 *   vignette            VignetteIntensity
 *   depthOfField        false: DepthOfFieldFocalDistance 0, which switches it off.
 *                       true: DepthOfFieldFocalDistance = 100·focusDistance cm and
 *                       DepthOfFieldFstop = fStop (5 m and f/2.8 when absent), on a
 *                       sensor 24 mm high. Absent, with or without focusDistance and
 *                       fStop: left to the level, as the web leaves it off
 *   chromaticAberration SceneFringeIntensity = 5·value (≈)
 *   grain               FilmGrainIntensity
 */
LOOMASCENESYNC_API FPostProcessSettings LoomaMakePostProcessSettings(const FLoomaPostProcessSpec& Look,
    float FrameAspect);

/** The sensor height the format reads a lens against: a full-frame gate. */
inline constexpr float LoomaSensorHeightMm = 24.0f;

/** A frame's aspect when nothing says otherwise: a full-frame still, 36 × 24. */
inline constexpr float LoomaDefaultFrameAspect = 1.5f;
