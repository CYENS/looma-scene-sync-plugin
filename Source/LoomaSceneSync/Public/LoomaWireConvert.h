#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Wire <-> UE transform conversion, shared by the scene-sync layer and the
 * generation-job parser.
 *
 * Wire: right-handed, Y-up, meters, quat [x,y,z,w]. UE: left-handed, Z-up, cm.
 * Must match glTFRuntime's DEFAULT SceneBasis (glTFRuntimeParser.h,
 * FglTFRuntimeConfig::GetMatrix), which maps glTF -> UE as
 *   (x, y, z) -> (-z, x, y)      [glTF -Z forward -> UE +X forward, Y-up -> Z-up]
 * so mesh geometry and actor transforms agree. The map flips handedness
 * (det = -1), so a rotation conjugates as: vector part through the axis map,
 * then negated; w unchanged:  q_UE = (qz, -qx, -qy, qw).
 */

/** Build a UE transform from a wire {p,q,s} JSON object (any field may be absent). */
LOOMASCENESYNC_API FTransform LoomaWireToUe(const TSharedPtr<FJsonObject>& T);

/** Build a wire {p,q,s} JSON object from a UE transform. */
LOOMASCENESYNC_API TSharedRef<FJsonObject> LoomaUeToWire(const FTransform& T);

/**
 * The same map for one point or one rotation, for maths done in the wire's own frame
 * (a path's curve, an aim) whose result has to land on an engine component. A point
 * goes from wire metres to UE centimetres; a rotation is conjugated as above, which
 * keeps products: converting R1·R2 is converting R1 times converting R2.
 */
LOOMASCENESYNC_API FVector LoomaWirePointToUe(const FVector& WireMetres);
LOOMASCENESYNC_API FVector LoomaUePointToWire(const FVector& UeCm);
LOOMASCENESYNC_API FQuat LoomaWireQuatToUe(const FQuat& Wire);
LOOMASCENESYNC_API FQuat LoomaUeQuatToWire(const FQuat& Ue);
