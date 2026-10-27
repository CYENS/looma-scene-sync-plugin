#include "LoomaCameraComponent.h"

namespace
{
/** Wire metres → UE centimetres. */
constexpr float CameraCmPerMetre = 100.0f;
} // namespace

float LoomaHorizontalFov(float VerticalFovDegrees, float Aspect)
{
    const double HalfVertical = FMath::DegreesToRadians(static_cast<double>(VerticalFovDegrees)) / 2.0;
    return static_cast<float>(FMath::RadiansToDegrees(2.0 * FMath::Atan(Aspect * FMath::Tan(HalfVertical))));
}

ULoomaCameraComponent::ULoomaCameraComponent()
{
    // A scene's camera is placed by the document. Without this an attached headset
    // would steer it, which is the player's view and not the shot.
    bLockToHmd = false;
    ApplySpec(Spec);
}

void ULoomaCameraComponent::ApplySpec(const FLoomaCameraSpec& InSpec)
{
    Spec = InSpec;
    const bool bOrtho = Spec.Projection == ELoomaCameraProjection::Orthographic;
    SetProjectionMode(bOrtho ? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective);

    // An aspect is a frame, not a stretch: Unreal letterboxes a constrained camera, and
    // the frame shows the camera's vertical angle across its own height.
    bConstrainAspectRatio = Spec.bHasAspect;
    AspectRatio = Spec.bHasAspect ? Spec.Aspect : AspectRatio;
    // Without a frame of its own, the vertical angle holds on whatever screen it is
    // shown on. Ignored while the aspect is constrained.
    bOverrideAspectRatioAxisConstraint = !Spec.bHasAspect;
    AspectRatioAxisConstraint = EAspectRatioAxisConstraint::AspectRatio_MaintainYFOV;

    FieldOfView = Spec.bHasAspect ? LoomaHorizontalFov(Spec.Fov, Spec.Aspect) : Spec.Fov;
    OrthoWidth = 2.0f * Spec.OrthoSize * CameraCmPerMetre * (Spec.bHasAspect ? Spec.Aspect : 1.0f);
    SetAutoCalculateOrthoPlanes(false);
    OrthoNearClipPlane = Spec.Near * CameraCmPerMetre;
    OrthoFarClipPlane = Spec.Far * CameraCmPerMetre;
}

void ULoomaCameraComponent::GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView)
{
    Super::GetCameraView(DeltaTime, DesiredView);
    // What UCineCameraComponent does with its CustomNearClippingPlane override. A plain
    // camera leaves this at -1, the engine-wide GNearClippingPlane.
    DesiredView.PerspectiveNearClipPlane = Spec.Near * CameraCmPerMetre;
}
