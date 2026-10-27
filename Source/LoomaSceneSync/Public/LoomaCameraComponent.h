#pragma once

#include "Camera/CameraComponent.h"
#include "CoreMinimal.h"
#include "LoomaSceneComponents.h"
#include "LoomaCameraComponent.generated.h"

/**
 * The `camera` component's lens on an Unreal camera (looma-xr-asset-demo/docs/
 * scene-format.md, "The `camera` component", whose engine table this follows).
 *
 * A UCameraComponent rather than a UCineCameraComponent, because the cine camera
 * rewrites FieldOfView from its filmback and overrides depth of field from its own
 * focus settings on every view, which would fight both the format's angle and the
 * `postProcess` component. It adds the one thing the plain camera lacks: a near plane
 * of its own, set on the view the way UCineCameraComponent's CustomNearClippingPlane
 * is.
 *
 *   `fov`        vertical, as the format stores it. With an `aspect`, FieldOfView is
 *                the horizontal angle that frame needs (LoomaHorizontalFov). Without
 *                one, FieldOfView is the vertical angle itself, read by the
 *                AspectRatio_MaintainYFOV constraint this camera overrides to, so the
 *                camera keeps its vertical angle on any screen.
 *   `aspect`     bConstrainAspectRatio and AspectRatio: the frame is letterboxed.
 *   `orthoSize`  half the height. Under MaintainYFOV an orthographic OrthoWidth is the
 *                view's full HEIGHT, so it is 200·orthoSize cm; with an `aspect` it is
 *                the full width, 200·orthoSize·aspect.
 *   `near`       the perspective view's near plane, 100·near cm; an orthographic
 *                view's OrthoNearClipPlane.
 *   `far`        an orthographic view's OrthoFarClipPlane. A perspective far plane is
 *                always at infinity in Unreal.
 *
 * The node looks down its local −Z, which LoomaWireConvert maps to UE +X, so the
 * camera sits on the node with no turn of its own.
 */
UCLASS(ClassGroup = Looma)
class LOOMASCENESYNC_API ULoomaCameraComponent : public UCameraComponent
{
    GENERATED_BODY()

public:
    ULoomaCameraComponent();

    /** Point the lens at `Spec`. Idempotent. */
    void ApplySpec(const FLoomaCameraSpec& Spec);

    /** The lens it was last given. */
    const FLoomaCameraSpec& GetSpec() const { return Spec; }

    virtual void GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView) override;

private:
    UPROPERTY(VisibleAnywhere, Category = "Looma")
    FLoomaCameraSpec Spec;
};

/**
 * The horizontal field of view, degrees, that shows `VerticalFovDegrees` across the
 * height of a frame `Aspect` wide: deg(2·atan(a·tan(rad(fov)/2))).
 */
LOOMASCENESYNC_API float LoomaHorizontalFov(float VerticalFovDegrees, float Aspect);
