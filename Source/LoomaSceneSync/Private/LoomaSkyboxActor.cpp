#include "LoomaSkyboxActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Components/SkyLightComponent.h"
#include "ImageCore.h"
#include "Math/Float16Color.h"
#include "UObject/UObjectIterator.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "LoomaSceneSyncLog.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ALoomaSkyboxActor::ALoomaSkyboxActor()
{
    Dome = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Panorama"));
    RootComponent = Dome;
    Dome->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Dome->SetCastShadow(false);
    Dome->bVisibleInReflectionCaptures = false;
    Dome->bVisibleInRealTimeSkyCaptures = false;
    Dome->bUseAsOccluder = false;
    Dome->SetBoundsScale(100000.0f);
    Dome->SetVisibility(false);
    EnvironmentLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("HDRLighting"));
    EnvironmentLight->SetupAttachment(Dome);
    EnvironmentLight->SetMobility(EComponentMobility::Movable);
    EnvironmentLight->SourceType = SLS_SpecifiedCubemap;
    // Use the entire panorama, including ground radiance, like the other clients.
    EnvironmentLight->bLowerHemisphereIsBlack = false;
    EnvironmentLight->SetVisibility(false);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Panorama(TEXT("/LoomaSceneSync/Materials/M_LoomaPanorama.M_LoomaPanorama"));
    Dome->SetStaticMesh(Sphere.Object);
    BaseMaterial = Panorama.Object;
    SetActorScale3D(FVector(10000.0));
}

void ALoomaSkyboxActor::Apply(const FString& Url, const FLoomaSkyboxSpec& Spec)
{
    const bool bChanged = !(CurrentSpec == Spec);
    CurrentSpec = Spec;
    if (!Material && BaseMaterial)
    {
        Material = UMaterialInstanceDynamic::Create(BaseMaterial, this);
        Dome->SetMaterial(0, Material);
        Material->SetScalarParameterValue(TEXT("TanHalfFov"), TanHalfFov);
    }
    if (Material)
    {
        Material->SetScalarParameterValue(TEXT("Yaw"), FMath::DegreesToRadians(Spec.Rotation));
        Material->SetScalarParameterValue(TEXT("Brightness"), FMath::Pow(2.0f, Spec.Exposure));
        Material->SetVectorParameterValue(TEXT("Tint"), Spec.Tint);
    }
    if (Url == CurrentUrl) { if (bChanged) UpdateLighting(); return; }
    CurrentUrl = Url;
    const int32 Version = ++RequestVersion;
    Dome->SetVisibility(false);
    Texture = nullptr;
    SourcePixels.Empty();
    ReleaseLighting();
    if (Url.IsEmpty() || !Material) return;
    const TWeakObjectPtr<ALoomaSkyboxActor> Weak(this);
    const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(Url);
    Request->SetVerb(TEXT("GET"));
    Request->OnProcessRequestComplete().BindLambda([Weak, Version](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
    {
        ALoomaSkyboxActor* Self = Weak.Get();
        if (!Self || Self->RequestVersion != Version) return;
        if (!bOk || !Response.IsValid() || Response->GetResponseCode() != 200)
        {
            UE_LOG(LogLoomaSync, Warning, TEXT("Skybox download failed: %s"), *Self->CurrentUrl);
            return;
        }
        FImage Image;
        const TArray<uint8>& Bytes = Response->GetContent();
        if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image)
            || Image.SizeX != Image.SizeY * 2 || Image.SizeX > 8192) return;
        Self->Texture = FImageUtils::CreateTexture2DFromImage(Image);
        if (!Self->Texture) return;
        Self->Texture->AddressX = TA_Wrap;
        Self->Texture->AddressY = TA_Clamp;
        Self->Texture->SRGB = Image.GammaSpace == EGammaSpace::sRGB;
        Self->Texture->UpdateResource();
        Self->Material->SetTextureParameterValue(TEXT("Panorama"), Self->Texture);
        Self->Dome->SetVisibility(true);
        FImage Linear;
        Image.ResizeTo(Linear, FMath::Min(Image.SizeX, 1024), FMath::Min(Image.SizeY, 512),
            ERawImageFormat::RGBA32F, EGammaSpace::Linear);
        Self->SourceWidth = Linear.SizeX; Self->SourceHeight = Linear.SizeY;
        Self->SourcePixels.SetNumUninitialized(Linear.SizeX * Linear.SizeY);
        FMemory::Memcpy(Self->SourcePixels.GetData(), Linear.RawData.GetData(), Self->SourcePixels.Num() * sizeof(FLinearColor));
        Self->UpdateLighting();
    });
    Request->ProcessRequest();
}

void ALoomaSkyboxActor::SetOrthographicFov(float VerticalFovDegrees)
{
    const float Tan = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(VerticalFovDegrees, 1.0f, 170.0f)) / 2.0f);
    if (FMath::IsNearlyEqual(Tan, TanHalfFov))
    {
        return; // called every frame
    }
    TanHalfFov = Tan;
    if (Material)
    {
        Material->SetScalarParameterValue(TEXT("TanHalfFov"), TanHalfFov);
    }
}

void ALoomaSkyboxActor::ReleaseLighting()
{
    EnvironmentLight->SetVisibility(false);
    EnvironmentLight->SetCubemap(nullptr);
    EnvironmentCube = nullptr;
    for (const auto& Previous : PreviousSkyLights) if (Previous.IsValid()) Previous->SetVisibility(true);
    PreviousSkyLights.Empty();
}

void ALoomaSkyboxActor::UpdateLighting()
{
    if (!CurrentSpec.bLighting || SourcePixels.IsEmpty()) { ReleaseLighting(); return; }
    constexpr int32 Size = 128;
    UTextureCube* Cube = UTextureCube::CreateTransient(Size, Size, PF_FloatRGBA);
    Cube->SRGB = false;
    auto* Pixels = static_cast<FFloat16Color*>(Cube->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
    const float Yaw = FMath::DegreesToRadians(CurrentSpec.Rotation);
    for (int32 Face = 0; Face < 6; ++Face) for (int32 Y = 0; Y < Size; ++Y) for (int32 X = 0; X < Size; ++X)
    {
        const float U = (X + 0.5f) / Size * 2 - 1, V = (Y + 0.5f) / Size * 2 - 1;
        FVector Native;
        switch (Face) {
            case 0: Native = FVector(1, -V, -U); break;
            case 1: Native = FVector(-1, -V, U); break;
            case 2: Native = FVector(U, 1, V); break;
            case 3: Native = FVector(U, -1, -V); break;
            case 4: Native = FVector(U, -V, 1); break;
            default: Native = FVector(-U, -V, -1); break;
        }
        FVector D(Native.Y, Native.Z, -Native.X); D.Normalize();
        const float DX = FMath::Cos(Yaw)*D.X - FMath::Sin(Yaw)*D.Z;
        const float DZ = FMath::Sin(Yaw)*D.X + FMath::Cos(Yaw)*D.Z;
        const float PX = (FMath::Atan2(DZ, DX) / (2*PI) + .5f) * SourceWidth - .5f;
        const float PY = (.5f - FMath::Asin(FMath::Clamp(D.Y, -1.0, 1.0)) / PI) * SourceHeight - .5f;
        const int32 IX = FMath::FloorToInt(PX), IY = FMath::FloorToInt(PY);
        const float FX = PX - IX, FY = PY - IY;
        auto Sample = [&](int32 SX, int32 SY) {
            return SourcePixels[FMath::Clamp(SY, 0, SourceHeight-1)*SourceWidth + (SX % SourceWidth + SourceWidth) % SourceWidth];
        };
        FLinearColor Color = FMath::Lerp(FMath::Lerp(Sample(IX,IY),Sample(IX+1,IY),FX),
            FMath::Lerp(Sample(IX,IY+1),Sample(IX+1,IY+1),FX),FY);
        Color = Color * CurrentSpec.Tint * FMath::Pow(2.0f, CurrentSpec.Exposure);
        Pixels[(Face*Size + Y)*Size + X] = FFloat16Color(Color);
    }
    Cube->GetPlatformData()->Mips[0].BulkData.Unlock();
    Cube->UpdateResource();
    // Own the global environment while active; restore existing sky lights on exit.
    if (!EnvironmentCube) for (TObjectIterator<USkyLightComponent> It; It; ++It)
        if (It->GetWorld() == GetWorld() && *It != EnvironmentLight && It->IsVisible())
        { PreviousSkyLights.Add(*It); It->SetVisibility(false); }
    EnvironmentCube = Cube;
    EnvironmentLight->SetIntensity(CurrentSpec.LightingIntensity);
    // SetCubemap queues filtering only for a visible sky light.
    EnvironmentLight->SetVisibility(true);
    EnvironmentLight->SetCubemap(Cube);
}

void ALoomaSkyboxActor::EndPlay(const EEndPlayReason::Type Reason)
{
    ++RequestVersion;
    ReleaseLighting();
    Super::EndPlay(Reason);
}
