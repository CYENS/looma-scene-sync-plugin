#include "LoomaSceneComponents.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
/** Wire vector → FVector, keeping wire axes. Short/absent input keeps `Fallback`. */
FVector WireVector(const TSharedPtr<FJsonObject>& Obj, const FString& Field, const FVector& Fallback)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Obj->TryGetArrayField(Field, Values) || !Values)
    {
        return Fallback;
    }
    FVector Out = Fallback;
    for (int32 I = 0; I < 3 && Values->IsValidIndex(I); ++I)
    {
        double Number = 0.0;
        if ((*Values)[I].IsValid() && (*Values)[I]->TryGetNumber(Number))
        {
            Out[I] = Number;
        }
    }
    return Out;
}

/** Read a float field, leaving `Value` untouched when the field is absent. */
void ReadFloat(const TSharedPtr<FJsonObject>& Obj, const FString& Field, float& Value)
{
    double Number = 0.0;
    if (Obj->TryGetNumberField(Field, Number))
    {
        Value = static_cast<float>(Number);
    }
}

void ReadColor(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FLinearColor& Value)
{
    FString Hex;
    if (Obj->TryGetStringField(Field, Hex))
    {
        Value = LoomaParseColor(Hex, Value);
    }
}

void ParseModel(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasModel = true;
    C->TryGetStringField(TEXT("assetId"), Out.Model.AssetId);
    // `url` is deliberately ignored — a native client rebuilds the path from
    // assetId, because the wire's url is the browser's /api-proxied one.
    C->TryGetStringField(TEXT("jobId"), Out.Model.JobId);
}

void ParseMesh(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasMesh = true;
    FString Shape;
    if (C->TryGetStringField(TEXT("shape"), Shape))
    {
        // Same repair the backend makes (scenegraph.MESH_SHAPES): an unrecognised
        // shape is a box, not a dropped component.
        if (Shape == TEXT("sphere"))        { Out.Mesh.Shape = ELoomaMeshShape::Sphere; }
        else if (Shape == TEXT("plane"))    { Out.Mesh.Shape = ELoomaMeshShape::Plane; }
        else if (Shape == TEXT("cylinder")) { Out.Mesh.Shape = ELoomaMeshShape::Cylinder; }
        else                                { Out.Mesh.Shape = ELoomaMeshShape::Box; }
    }
    Out.Mesh.Size = WireVector(C, TEXT("size"), Out.Mesh.Size);
}

void ParseMaterial(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasMaterial = true;
    ReadColor(C, TEXT("color"), Out.Material.Color);
    ReadFloat(C, TEXT("roughness"), Out.Material.Roughness);
    ReadFloat(C, TEXT("metalness"), Out.Material.Metalness);
}

void ParseLight(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasLight = true;
    FString Type;
    if (C->TryGetStringField(TEXT("lightType"), Type))
    {
        if (Type == TEXT("spot"))             { Out.Light.LightType = ELoomaLightType::Spot; }
        else if (Type == TEXT("directional")) { Out.Light.LightType = ELoomaLightType::Directional; }
        else                                  { Out.Light.LightType = ELoomaLightType::Point; }
    }
    ReadColor(C, TEXT("color"), Out.Light.Color);
    ReadFloat(C, TEXT("intensity"), Out.Light.Intensity);
    ReadFloat(C, TEXT("distance"), Out.Light.Distance);
    ReadFloat(C, TEXT("angle"), Out.Light.Angle);
    ReadFloat(C, TEXT("penumbra"), Out.Light.Penumbra);
    C->TryGetBoolField(TEXT("castShadow"), Out.Light.bCastShadow);
}

void ParseAnimation(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasAnimation = true;
    // `clip: null` fails TryGetStringField and leaves Clip empty, which is exactly
    // what null means here: no clip, stand at rest.
    C->TryGetStringField(TEXT("clip"), Out.Animation.Clip);
    C->TryGetBoolField(TEXT("playing"), Out.Animation.bPlaying);
}

/** Smallest `minDistance`, in metres: the backend's AUDIO_MIN_DISTANCE. */
constexpr float AudioMinDistance = 0.01f;

void ParseAudio(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasAudio = true;
    FLoomaAudioSpec& Audio = Out.Audio;
    C->TryGetStringField(TEXT("assetId"), Audio.AssetId);
    // `url` is ignored, as a model's is: it is the browser's /api-proxied path.
    C->TryGetBoolField(TEXT("playing"), Audio.bPlaying);
    C->TryGetBoolField(TEXT("loop"), Audio.bLoop);
    C->TryGetBoolField(TEXT("spatial"), Audio.bSpatial);
    ReadFloat(C, TEXT("volume"), Audio.Volume);
    ReadFloat(C, TEXT("pitch"), Audio.Pitch);
    ReadFloat(C, TEXT("minDistance"), Audio.MinDistance);
    ReadFloat(C, TEXT("maxDistance"), Audio.MaxDistance);
    FString Rolloff;
    if (C->TryGetStringField(TEXT("rolloff"), Rolloff))
    {
        Audio.Rolloff = Rolloff == TEXT("linear") ? ELoomaAudioRolloff::Linear : ELoomaAudioRolloff::Inverse;
    }
    // The backend's own repairs (scenegraph._clean_audio), made again here because the
    // wire is repaired, never rejected: an unknown rolloff is inverse, and the numbers
    // are clamped into what every client can play.
    Audio.Volume = FMath::Clamp(Audio.Volume, 0.0f, 1.0f);
    Audio.Pitch = FMath::Clamp(Audio.Pitch, 0.5f, 2.0f);
    Audio.MinDistance = FMath::Max(Audio.MinDistance, AudioMinDistance);
    Audio.MaxDistance = FMath::Max(Audio.MaxDistance, Audio.MinDistance);
}

/** A JSON number in `Field`, or false: absent, null, a string or anything else. */
bool ReadNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Field, double& Out)
{
    const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
    if (!Value.IsValid() || Value->Type != EJson::Number)
    {
        return false;
    }
    Out = Value->AsNumber();
    return FMath::IsFinite(Out);
}

/**
 * Only a literal JSON true. TryGetBoolField would also read "true" and 1, which the
 * web client (`=== true`) does not.
 */
bool IsLiteralTrue(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
{
    const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
    return Value.IsValid() && Value->Type == EJson::Boolean && Value->AsBool();
}

bool IsLiteralFalse(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
{
    const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
    return Value.IsValid() && Value->Type == EJson::Boolean && !Value->AsBool();
}

/** Exactly three numbers, or false. A path point or an offset is all or nothing. */
bool ReadTriple(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!Value.IsValid() || !Value->TryGetArray(Items) || !Items || Items->Num() != 3)
    {
        return false;
    }
    for (int32 I = 0; I < 3; ++I)
    {
        const TSharedPtr<FJsonValue>& Item = (*Items)[I];
        if (!Item.IsValid() || Item->Type != EJson::Number || !FMath::IsFinite(Item->AsNumber()))
        {
            return false;
        }
        Out[I] = Item->AsNumber();
    }
    return true;
}

// The hub's bounds (backend/app/scenegraph.py), applied again here for the same
// reason ParseAudio applies its own: the wire is repaired, never rejected.
constexpr double CameraMinFov = 1.0;
constexpr double CameraMaxFov = 170.0;
constexpr double CameraMinClip = 0.001;
constexpr double CameraMinAspect = 0.1;
constexpr double CameraMaxAspect = 10.0;
constexpr int32 PathMaxPoints = 256;
constexpr double PathMinDuration = 0.1;
constexpr double PathMaxDuration = 86400.0;

void ParseCamera(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasCamera = true;
    FLoomaCameraSpec& Camera = Out.Camera;
    FString Projection;
    if (C->TryGetStringField(TEXT("projection"), Projection))
    {
        Camera.Projection = Projection == TEXT("orthographic")
            ? ELoomaCameraProjection::Orthographic : ELoomaCameraProjection::Perspective;
    }
    double Number = 0.0;
    if (ReadNumber(C, TEXT("fov"), Number))
    {
        Camera.Fov = static_cast<float>(FMath::Clamp(Number, CameraMinFov, CameraMaxFov));
    }
    if (ReadNumber(C, TEXT("orthoSize"), Number))
    {
        Camera.OrthoSize = static_cast<float>(FMath::Max(Number, CameraMinClip));
    }
    if (ReadNumber(C, TEXT("near"), Number))
    {
        Camera.Near = static_cast<float>(FMath::Max(Number, CameraMinClip));
    }
    if (ReadNumber(C, TEXT("far"), Number))
    {
        Camera.Far = static_cast<float>(Number);
    }
    // Beyond the near plane, or the frustum is empty. The default far is held to it as
    // well, as lens.js does: a near plane past 1000 m pushes it out too.
    Camera.Far = FMath::Max(Camera.Far, Camera.Near * 1.001f);
    // `aspect: null` is the same as no aspect: whatever the screen is.
    if (ReadNumber(C, TEXT("aspect"), Number))
    {
        Camera.bHasAspect = true;
        Camera.Aspect = static_cast<float>(FMath::Clamp(Number, CameraMinAspect, CameraMaxAspect));
    }
    Camera.bPrimary = IsLiteralTrue(C, TEXT("primary"));
}

void ParsePath(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasPath = true;
    FLoomaPathSpec& Path = Out.Path;
    const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
    if (C->TryGetArrayField(TEXT("points"), Points) && Points)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Points)
        {
            FVector Point;
            // A point that is not three numbers is dropped, not zeroed: a stray
            // [0, 0, 0] would bend the curve through the node's origin.
            if (Path.Points.Num() < PathMaxPoints && ReadTriple(Value, Point))
            {
                Path.Points.Add(Point);
            }
        }
    }
    Path.bClosed = IsLiteralTrue(C, TEXT("closed"));
    Path.bPlaying = !IsLiteralFalse(C, TEXT("playing"));
    double Number = 0.0;
    if (ReadNumber(C, TEXT("duration"), Number))
    {
        Path.Duration = static_cast<float>(FMath::Clamp(Number, PathMinDuration, PathMaxDuration));
    }
    FString Name;
    if (C->TryGetStringField(TEXT("mode"), Name))
    {
        Path.Mode = Name == TEXT("pingpong") ? ELoomaPathMode::PingPong
            : Name == TEXT("once") ? ELoomaPathMode::Once : ELoomaPathMode::Loop;
    }
    if (C->TryGetStringField(TEXT("ease"), Name))
    {
        Path.Ease = Name == TEXT("inOut") ? ELoomaPathEase::InOut : ELoomaPathEase::Linear;
    }
    if (C->TryGetStringField(TEXT("orient"), Name))
    {
        Path.Orient = Name == TEXT("path") ? ELoomaPathOrient::Path : ELoomaPathOrient::Fixed;
    }
    Path.bStamped = ReadNumber(C, TEXT("startedAt"), Path.StartedAt);
    if (!ReadNumber(C, TEXT("offset"), Path.Offset))
    {
        Path.Offset = 0.0;
    }
}

void ParseLookAt(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasLookAt = true;
    // `target: null` fails TryGetStringField and leaves Target empty, which is what
    // null means: aim at nothing.
    C->TryGetStringField(TEXT("target"), Out.LookAt.Target);
    FString Axis;
    if (C->TryGetStringField(TEXT("axis"), Axis))
    {
        Out.LookAt.Axis = Axis == TEXT("-y") ? ELoomaLookAtAxis::MinusY : ELoomaLookAtAxis::MinusZ;
    }
    FVector Offset;
    if (ReadTriple(C->TryGetField(TEXT("offset")), Offset))
    {
        Out.LookAt.Offset = Offset;
    }
}

/** One post-process number: present, clamped, and flagged, or absent and untouched. */
void ReadSetting(const TSharedPtr<FJsonObject>& C, const TCHAR* Field, double Low, double High, bool& bHas,
    float& Value)
{
    double Number = 0.0;
    if (ReadNumber(C, Field, Number))
    {
        bHas = true;
        Value = static_cast<float>(FMath::Clamp(Number, Low, High));
    }
}

void ParsePostProcess(const TSharedPtr<FJsonObject>& C, FLoomaNodeComponents& Out)
{
    Out.bHasPostProcess = true;
    FLoomaPostProcessSpec& Look = Out.PostProcess;
    // Unlike everything above, a setting nobody can read is ABSENT rather than given a
    // default: here absence means "this volume does not touch it", and any value put in
    // its place would override whatever is underneath (`_clean_post_process`).
    bool bHasPriority = false;
    ReadSetting(C, TEXT("priority"), -1000000.0, 1000000.0, bHasPriority, Look.Priority);
    ReadSetting(C, TEXT("exposure"), -10.0, 10.0, Look.bHasExposure, Look.Exposure);
    ReadSetting(C, TEXT("contrast"), -1.0, 1.0, Look.bHasContrast, Look.Contrast);
    ReadSetting(C, TEXT("saturation"), -1.0, 1.0, Look.bHasSaturation, Look.Saturation);
    ReadSetting(C, TEXT("bloomIntensity"), 0.0, 10.0, Look.bHasBloomIntensity, Look.BloomIntensity);
    ReadSetting(C, TEXT("bloomThreshold"), 0.0, 100.0, Look.bHasBloomThreshold, Look.BloomThreshold);
    ReadSetting(C, TEXT("vignette"), 0.0, 1.0, Look.bHasVignette, Look.Vignette);
    ReadSetting(C, TEXT("focusDistance"), 0.01, 10000.0, Look.bHasFocusDistance, Look.FocusDistance);
    ReadSetting(C, TEXT("fStop"), 0.5, 64.0, Look.bHasFStop, Look.FStop);
    ReadSetting(C, TEXT("chromaticAberration"), 0.0, 1.0, Look.bHasChromaticAberration, Look.ChromaticAberration);
    ReadSetting(C, TEXT("grain"), 0.0, 1.0, Look.bHasGrain, Look.Grain);

    FString Name;
    if (C->TryGetStringField(TEXT("toneMapping"), Name))
    {
        Look.bHasToneMapping = true;
        if (Name == TEXT("none"))         { Look.ToneMapping = ELoomaToneMapping::None; }
        else if (Name == TEXT("aces"))    { Look.ToneMapping = ELoomaToneMapping::Aces; }
        else if (Name == TEXT("agx"))     { Look.ToneMapping = ELoomaToneMapping::Agx; }
        else if (Name == TEXT("neutral")) { Look.ToneMapping = ELoomaToneMapping::Neutral; }
        else                              { Look.bHasToneMapping = false; }
    }
    if (C->TryGetStringField(TEXT("colorFilter"), Name))
    {
        // A colour that is not one is absent, for the reason above, rather than white.
        const FLinearColor Unread(-1.0f, -1.0f, -1.0f);
        const FLinearColor Filter = LoomaParseColor(Name, Unread);
        if (Filter != Unread)
        {
            Look.bHasColorFilter = true;
            Look.ColorFilter = Filter;
        }
    }
    const TSharedPtr<FJsonValue> DepthOfField = C->TryGetField(TEXT("depthOfField"));
    if (DepthOfField.IsValid() && DepthOfField->Type == EJson::Boolean)
    {
        Look.bHasDepthOfField = true;
        Look.bDepthOfField = DepthOfField->AsBool();
    }
}
} // namespace

void FLoomaPostProcessSpec::Overlay(const FLoomaPostProcessSpec& Above)
{
    auto Take = [](bool bAbove, const auto& AboveValue, bool& bHas, auto& Value) {
        if (bAbove)
        {
            bHas = true;
            Value = AboveValue;
        }
    };
    Take(Above.bHasExposure, Above.Exposure, bHasExposure, Exposure);
    Take(Above.bHasToneMapping, Above.ToneMapping, bHasToneMapping, ToneMapping);
    Take(Above.bHasContrast, Above.Contrast, bHasContrast, Contrast);
    Take(Above.bHasSaturation, Above.Saturation, bHasSaturation, Saturation);
    Take(Above.bHasColorFilter, Above.ColorFilter, bHasColorFilter, ColorFilter);
    Take(Above.bHasBloomIntensity, Above.BloomIntensity, bHasBloomIntensity, BloomIntensity);
    Take(Above.bHasBloomThreshold, Above.BloomThreshold, bHasBloomThreshold, BloomThreshold);
    Take(Above.bHasVignette, Above.Vignette, bHasVignette, Vignette);
    Take(Above.bHasDepthOfField, Above.bDepthOfField, bHasDepthOfField, bDepthOfField);
    Take(Above.bHasFocusDistance, Above.FocusDistance, bHasFocusDistance, FocusDistance);
    Take(Above.bHasFStop, Above.FStop, bHasFStop, FStop);
    Take(Above.bHasChromaticAberration, Above.ChromaticAberration, bHasChromaticAberration, ChromaticAberration);
    Take(Above.bHasGrain, Above.Grain, bHasGrain, Grain);
}

bool FLoomaPostProcessSpec::operator==(const FLoomaPostProcessSpec& Other) const
{
    auto Same = [](bool bA, float A, bool bB, float B) { return bA == bB && (!bA || FMath::IsNearlyEqual(A, B)); };
    return FMath::IsNearlyEqual(Priority, Other.Priority)
        && Same(bHasExposure, Exposure, Other.bHasExposure, Other.Exposure)
        && bHasToneMapping == Other.bHasToneMapping && (!bHasToneMapping || ToneMapping == Other.ToneMapping)
        && Same(bHasContrast, Contrast, Other.bHasContrast, Other.Contrast)
        && Same(bHasSaturation, Saturation, Other.bHasSaturation, Other.Saturation)
        && bHasColorFilter == Other.bHasColorFilter && (!bHasColorFilter || ColorFilter.Equals(Other.ColorFilter, 1e-6f))
        && Same(bHasBloomIntensity, BloomIntensity, Other.bHasBloomIntensity, Other.BloomIntensity)
        && Same(bHasBloomThreshold, BloomThreshold, Other.bHasBloomThreshold, Other.BloomThreshold)
        && Same(bHasVignette, Vignette, Other.bHasVignette, Other.Vignette)
        && bHasDepthOfField == Other.bHasDepthOfField && (!bHasDepthOfField || bDepthOfField == Other.bDepthOfField)
        && Same(bHasFocusDistance, FocusDistance, Other.bHasFocusDistance, Other.FocusDistance)
        && Same(bHasFStop, FStop, Other.bHasFStop, Other.FStop)
        && Same(bHasChromaticAberration, ChromaticAberration, Other.bHasChromaticAberration, Other.ChromaticAberration)
        && Same(bHasGrain, Grain, Other.bHasGrain, Other.Grain);
}

FLoomaNodeComponents LoomaParseComponents(const TArray<TSharedPtr<FJsonValue>>* Components)
{
    FLoomaNodeComponents Out;
    if (!Components)
    {
        return Out; // no `components` key at all: an empty object
    }

    for (const TSharedPtr<FJsonValue>& Value : *Components)
    {
        const TSharedPtr<FJsonObject> C = Value.IsValid() ? Value->AsObject() : nullptr;
        FString Type;
        if (!C.IsValid() || !C->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
        {
            continue; // unaddressable — dropped, exactly as the backend drops it
        }

        if (Type == TEXT("hidden")) { Out.bHidden = true; continue; }
        // Ahead of the switch: a camera that is off still claims the node's postProcess.
        if (Type == TEXT("camera")) { Out.bCarriesCamera = true; }
        // Only a literal false switches a component off (scene-format.md). Keep
        // its definition in the actor's WireComponents, but omit its runtime spec
        // so the existing removal paths stop it without touching the children.
        // TryGetBoolField alone would also coerce 0 and "false" into an off switch.
        const TSharedPtr<FJsonValue> Enabled = C->TryGetField(TEXT("enabled"));
        if (Enabled.IsValid() && Enabled->Type == EJson::Boolean && !Enabled->AsBool())
        {
            continue;
        }

        if (Type == TEXT("skybox"))
        {
            Out.bHasSkybox = true;
            C->TryGetStringField(TEXT("assetId"), Out.Skybox.AssetId);
            C->TryGetStringField(TEXT("url"), Out.Skybox.Url);
            ReadFloat(C, TEXT("rotation"), Out.Skybox.Rotation);
            ReadFloat(C, TEXT("exposure"), Out.Skybox.Exposure);
            Out.Skybox.Rotation = FMath::IsFinite(Out.Skybox.Rotation) ? FMath::Fmod(Out.Skybox.Rotation, 360.0f) : 0;
            Out.Skybox.Exposure = FMath::IsFinite(Out.Skybox.Exposure) ? FMath::Clamp(Out.Skybox.Exposure, -8.0f, 8.0f) : 0;
            ReadColor(C, TEXT("tint"), Out.Skybox.Tint);
            C->TryGetBoolField(TEXT("lighting"), Out.Skybox.bLighting);
            ReadFloat(C, TEXT("lightingIntensity"), Out.Skybox.LightingIntensity);
            Out.Skybox.LightingIntensity = FMath::IsFinite(Out.Skybox.LightingIntensity)
                ? FMath::Clamp(Out.Skybox.LightingIntensity, 0.0f, 16.0f) : 1;
        }
        else if (Type == TEXT("model"))    { ParseModel(C, Out); }
        else if (Type == TEXT("mesh"))     { ParseMesh(C, Out); }
        else if (Type == TEXT("material")) { ParseMaterial(C, Out); }
        else if (Type == TEXT("light"))    { ParseLight(C, Out); }
        else if (Type == TEXT("animation")) { ParseAnimation(C, Out); }
        else if (Type == TEXT("audio"))    { ParseAudio(C, Out); }
        else if (Type == TEXT("camera"))   { ParseCamera(C, Out); }
        else if (Type == TEXT("path"))     { ParsePath(C, Out); }
        else if (Type == TEXT("lookAt"))   { ParseLookAt(C, Out); }
        else if (Type == TEXT("postProcess")) { ParsePostProcess(C, Out); }
        else if (Type == TEXT("livelink"))
        {
            C->TryGetStringField(TEXT("subject"), Out.LiveLinkSubject);
            C->TryGetStringField(TEXT("source"), Out.LiveLinkSource);
            C->TryGetStringField(TEXT("stream"), Out.LiveLinkStream);
        }
        else
        {
            // Skipped, but the node lives on. This is what makes a new component
            // type an additive, one-side-at-a-time deploy.
            Out.UnknownTypes.AddUnique(Type);
        }
    }
    return Out;
}

FLinearColor LoomaParseColor(const FString& Hex, const FLinearColor& Fallback)
{
    // FColor::FromHex tolerates a leading '#' and expands #rgb, but reads any
    // non-hex digit as 0 — so validate the shape first rather than rendering
    // "chartreuse" as black.
    const int32 Start = (!Hex.IsEmpty() && Hex[0] == TCHAR('#')) ? 1 : 0;
    const int32 Digits = Hex.Len() - Start;
    if (Digits != 3 && Digits != 6)
    {
        return Fallback;
    }
    for (int32 I = Start; I < Hex.Len(); ++I)
    {
        if (!FChar::IsHexDigit(Hex[I]))
        {
            return Fallback;
        }
    }
    // The wire carries CSS-style sRGB hex, which three.js also decodes as sRGB —
    // so the same de-gamma has to happen here or every colour reads too bright.
    return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
}

TSharedRef<FJsonObject> LoomaMakeModelComponent(const FString& AssetId, const FString& JobId,
    const FString& WebUrl)
{
    TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("type"), TEXT("model"));
    Out->SetStringField(TEXT("assetId"), AssetId);
    if (!WebUrl.IsEmpty())
    {
        // For the browser, which cannot rebuild this path for itself. See the header.
        Out->SetStringField(TEXT("url"), WebUrl);
    }
    if (!JobId.IsEmpty())
    {
        // On the component, not on the node: the hub keeps a known component's extra
        // keys but strips every node-level field it doesn't know (see FLoomaModelSpec).
        Out->SetStringField(TEXT("jobId"), JobId);
    }
    return Out;
}

TArray<TSharedPtr<FJsonValue>> LoomaWithLiveLinkSubject(
    const TArray<TSharedPtr<FJsonValue>>& Components, const FString& Subject, const FString& Source,
    const FString& Stream)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const TSharedPtr<FJsonValue>& Value : Components)
    {
        const TSharedPtr<FJsonObject> Component = Value.IsValid() ? Value->AsObject() : nullptr;
        FString Type;
        if (Component.IsValid() && Component->TryGetStringField(TEXT("type"), Type) && Type == TEXT("livelink"))
        {
            continue; // a type occurs once per node: the old one goes, whatever it named
        }
        Out.Add(Value);
    }
    if (!Subject.IsEmpty())
    {
        TSharedRef<FJsonObject> LiveLink = MakeShared<FJsonObject>();
        LiveLink->SetStringField(TEXT("type"), TEXT("livelink"));
        LiveLink->SetStringField(TEXT("subject"), Subject);
        if (!Source.IsEmpty())
        {
            LiveLink->SetStringField(TEXT("source"), Source);
            if (!Stream.IsEmpty())
            {
                LiveLink->SetStringField(TEXT("stream"), Stream);
            }
        }
        Out.Add(MakeShared<FJsonValueObject>(LiveLink));
    }
    return Out;
}
