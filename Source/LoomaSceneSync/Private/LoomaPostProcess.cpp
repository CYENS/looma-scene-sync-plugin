#include "LoomaPostProcess.h"

namespace
{
/** Wire metres → UE centimetres. */
constexpr float LookCmPerMetre = 100.0f;

/**
 * Unreal's fringe at 5 looks like the web's strongest aberration. Matched by eye, as
 * the format allows for this setting.
 */
constexpr float FringePerUnit = 5.0f;
} // namespace

bool LoomaResolveLook(TArray<FLoomaLookVolume> Globals, const FLoomaPostProcessSpec* CameraOwn,
    FLoomaPostProcessSpec& OutLook)
{
    Globals.StableSort([](const FLoomaLookVolume& A, const FLoomaLookVolume& B) {
        return A.Spec.Priority != B.Spec.Priority ? A.Spec.Priority < B.Spec.Priority : A.Order < B.Order;
    });
    OutLook = FLoomaPostProcessSpec();
    bool bAny = false;
    for (const FLoomaLookVolume& Volume : Globals)
    {
        OutLook.Overlay(Volume.Spec);
        bAny = true;
    }
    if (CameraOwn)
    {
        OutLook.Overlay(*CameraOwn);
        bAny = true;
    }
    return bAny;
}

FPostProcessSettings LoomaMakePostProcessSettings(const FLoomaPostProcessSpec& Look, float FrameAspect)
{
    FPostProcessSettings Out;
    if (Look.bHasExposure)
    {
        // A fixed exposure, shifted by the stops: what `rgb *= 2^exposure` is on a
        // renderer whose exposure otherwise meters itself.
        Out.bOverride_AutoExposureMethod = true;
        Out.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        Out.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
        Out.AutoExposureApplyPhysicalCameraExposure = false;
        Out.bOverride_AutoExposureBias = true;
        Out.AutoExposureBias = Look.Exposure;
    }
    if (Look.bHasToneMapping)
    {
        Out.bOverride_ToneCurveAmount = true;
        Out.ToneCurveAmount = Look.ToneMapping == ELoomaToneMapping::None ? 0.0f : 1.0f;
    }
    if (Look.bHasContrast)
    {
        const float C = 1.0f + Look.Contrast;
        Out.bOverride_ColorContrast = true;
        Out.ColorContrast = FVector4(C, C, C, 1.0f);
    }
    if (Look.bHasSaturation)
    {
        const float S = 1.0f + Look.Saturation;
        Out.bOverride_ColorSaturation = true;
        Out.ColorSaturation = FVector4(S, S, S, 1.0f);
    }
    if (Look.bHasColorFilter)
    {
        Out.bOverride_ColorGain = true;
        Out.ColorGain = FVector4(Look.ColorFilter.R, Look.ColorFilter.G, Look.ColorFilter.B, 1.0f);
    }
    if (Look.bHasBloomIntensity)
    {
        Out.bOverride_BloomIntensity = true;
        Out.BloomIntensity = Look.BloomIntensity;
    }
    if (Look.bHasBloomThreshold)
    {
        Out.bOverride_BloomThreshold = true;
        Out.BloomThreshold = Look.BloomThreshold;
    }
    if (Look.bHasVignette)
    {
        Out.bOverride_VignetteIntensity = true;
        Out.VignetteIntensity = Look.Vignette;
    }
    if (Look.bHasDepthOfField)
    {
        Out.bOverride_DepthOfFieldFocalDistance = true;
        if (!Look.bDepthOfField)
        {
            // Unreal's depth of field is off at a focal distance of 0.
            Out.DepthOfFieldFocalDistance = 0.0f;
        }
        else
        {
            Out.DepthOfFieldFocalDistance = Look.FocusDistance * LookCmPerMetre;
            Out.bOverride_DepthOfFieldFstop = true;
            Out.DepthOfFieldFstop = Look.FStop;
            // Unreal works the lens length out from the view's horizontal angle and the
            // sensor's width. A gate 24 mm high and as wide as the frame gives the
            // length the format means: the viewing camera's vertical angle on full frame.
            Out.bOverride_DepthOfFieldSensorWidth = true;
            Out.DepthOfFieldSensorWidth = LoomaSensorHeightMm * (FrameAspect > 0.0f ? FrameAspect : LoomaDefaultFrameAspect);
        }
    }
    if (Look.bHasChromaticAberration)
    {
        Out.bOverride_SceneFringeIntensity = true;
        Out.SceneFringeIntensity = Look.ChromaticAberration * FringePerUnit;
    }
    if (Look.bHasGrain)
    {
        Out.bOverride_FilmGrainIntensity = true;
        Out.FilmGrainIntensity = Look.Grain;
    }
    return Out;
}
