#pragma once

#include "CoreMinimal.h"
#include "LoomaSceneComponents.h"

/**
 * What moves on its own, every frame: carriages along `path`s and aims turned by
 * `lookAt`s (looma-xr-asset-demo/docs/scene-format.md, "The `path` component" and
 * "The `lookAt` component"). Pure maths, in the **wire's own frame** (right-handed,
 * Y-up, metres), so it can be held to the web client's numbers
 * (frontend/src/scene/path/curve.js, frontend/src/scene/lookAt.js) by a test. The
 * actor converts the result through LoomaWireConvert.
 *
 * **The curve** is a centripetal Catmull-Rom spline (alpha ½) through every point,
 * three.js's `CatmullRomCurve3` with `curveType: "centripetal"`, evaluated by the
 * spec's own formula rather than by Unreal's USplineComponent, whose `CurveAuto` is
 * the uniform kind and whose ends differ.
 *
 * **Length is the format's table, not Unreal's**: 64 samples per segment, evenly
 * spaced in each segment's own parameter, joined by straight lines, and a distance
 * fraction mapped to the curve's parameter by linear interpolation between samples.
 * Every engine has its own table (three.js 200 samples, Unreal 10 per segment,
 * Cinemachine 30), which would put one camera in slightly different places on each.
 */

/** Samples per segment in the format's length table. */
inline constexpr int32 LoomaPathSamplesPerSegment = 64;

/** A path's curve and its length table. */
class LOOMASCENESYNC_API FLoomaPathCurve
{
public:
    /**
     * The curve through `Points` (wire metres). With fewer than two it is not a curve
     * (IsCurve false): the carriage then stands on the one point, or on the origin. A
     * closed curve needs three points; with two it is open.
     */
    FLoomaPathCurve(const TArray<FVector>& Points, bool bClosed);

    bool IsCurve() const { return Points.Num() >= 2; }
    bool IsClosed() const { return bClosed; }
    int32 NumSegments() const;

    /** The table's length: the polyline through every sample, in wire metres. */
    double GetLength() const { return Lengths.Num() > 0 ? Lengths.Last() : 0.0; }

    /** The curve's own parameter (0…1, even per segment) at distance fraction `U`. */
    double ParameterAt(double U) const;

    /** The point at curve parameter `T`: three.js's `getPoint`. */
    FVector PointAtParameter(double T) const;

    /**
     * The unit direction at curve parameter `T`: three.js's `getTangent`, a central
     * difference of 1e-4 in `T`, clamped to 0…1. Zero where the curve does not move.
     */
    FVector TangentAtParameter(double T) const;

    FVector PointAt(double U) const { return PointAtParameter(ParameterAt(U)); }
    FVector TangentAt(double U) const { return TangentAtParameter(ParameterAt(U)); }

    /**
     * Segment `Index` as Hermite tangents (m1 out of its first point, m2 into its
     * second), which is how an engine whose own spline is not this one draws it
     * exactly: Unreal's `CurveCustomTangent` leave/arrive tangents, or a cubic Bezier
     * with handles m1/3 and −m2/3.
     */
    void SegmentTangents(int32 Index, FVector& OutM1, FVector& OutM2) const;

private:
    /** The segment's four control points: the neighbours reflected at an open end. */
    void SegmentPoints(int32 Index, FVector& P0, FVector& P1, FVector& P2, FVector& P3) const;

    TArray<FVector> Points;
    bool bClosed = false;
    /** Cumulative polyline length at each sample, NumSegments × 64 + 1 of them. */
    TArray<double> Lengths;
};

/**
 * Seconds of travel at `NowSeconds` on the hub's clock, unfolded: the hub's
 * `path_time`. Paused, or not stamped by the hub yet, it is the offset.
 */
LOOMASCENESYNC_API double LoomaPathTime(const FLoomaPathSpec& Path, double NowSeconds);

/** Seconds of travel folded into one traversal by the path's mode, before easing. */
LOOMASCENESYNC_API double LoomaPathFoldedTime(const FLoomaPathSpec& Path, double Time);

/** How far along the curve the carriage is at `Time`, 0…1 by distance, eased. */
LOOMASCENESYNC_API double LoomaPathProgress(const FLoomaPathSpec& Path, double Time);

/**
 * The carriage's pose at distance fraction `U`, in the node's frame (wire metres and
 * axes). `Fixed` keeps the node's own axes; `Path` turns the carriage so its −Z
 * points along the curve with +Y as near the node's +Y as it can be.
 */
LOOMASCENESYNC_API void LoomaCarriageAt(const FLoomaPathSpec& Path, const FLoomaPathCurve& Curve, double U,
    FVector& OutPosition, FQuat& OutRotation);

/**
 * three.js's `Matrix4.lookAt(eye, target, up)` as a rotation, in the wire's frame: the
 * one whose −Z runs from `Eye` to `Target` with +Y towards `Up`. Its fallbacks too:
 * coincident points look down −Z, and a direction parallel to `Up` is nudged by 1e-4.
 */
LOOMASCENESYNC_API FQuat LoomaWireLookRotation(const FVector& Eye, const FVector& Target, const FVector& Up);

/**
 * The world rotation (wire frame) that points `Axis` from `Eye` at `Target`, with
 * world +Y up so it never rolls. False when the two are the same point and there is
 * no direction: the node then keeps the rotation it had.
 */
LOOMASCENESYNC_API bool LoomaAimRotation(const FVector& Eye, const FVector& Target, ELoomaLookAtAxis Axis,
    FQuat& OutRotation);
