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
} // namespace

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

        if (Type == TEXT("model"))         { ParseModel(C, Out); }
        else if (Type == TEXT("mesh"))     { ParseMesh(C, Out); }
        else if (Type == TEXT("material")) { ParseMaterial(C, Out); }
        else if (Type == TEXT("light"))    { ParseLight(C, Out); }
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

TSharedRef<FJsonObject> LoomaMakeModelComponent(const FString& AssetId, const FString& JobId)
{
    TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("type"), TEXT("model"));
    Out->SetStringField(TEXT("assetId"), AssetId);
    if (!JobId.IsEmpty())
    {
        // On the component, not on the node: the hub keeps a known component's extra
        // keys but strips every node-level field it doesn't know (see FLoomaModelSpec).
        Out->SetStringField(TEXT("jobId"), JobId);
    }
    return Out;
}
