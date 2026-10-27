#include "LoomaDrivers.h"

namespace
{
/** Below this, a centripetal knot interval is two points on top of each other. */
constexpr double KnotEpsilon = 1e-4;

/** three.js's `Curve.getTangent` step, in the curve's whole parameter. */
constexpr double TangentDelta = 1e-4;

/** The wire's up. */
const FVector WireUp(0.0, 1.0, 0.0);
} // namespace

FLoomaPathCurve::FLoomaPathCurve(const TArray<FVector>& InPoints, bool bInClosed)
    : Points(InPoints)
    , bClosed(bInClosed && InPoints.Num() > 2)
{
    if (!IsCurve())
    {
        return;
    }
    // Sampled through PointAtParameter at j / n, as curve.js samples three's getPoint,
    // so both clients walk exactly the same polyline.
    const int32 N = NumSegments() * LoomaPathSamplesPerSegment;
    Lengths.SetNumUninitialized(N + 1);
    Lengths[0] = 0.0;
    FVector Previous = PointAtParameter(0.0);
    for (int32 J = 1; J <= N; ++J)
    {
        const FVector Next = PointAtParameter(static_cast<double>(J) / N);
        Lengths[J] = Lengths[J - 1] + FVector::Distance(Next, Previous);
        Previous = Next;
    }
}

int32 FLoomaPathCurve::NumSegments() const
{
    if (!IsCurve())
    {
        return 0;
    }
    return bClosed ? Points.Num() : Points.Num() - 1;
}

double FLoomaPathCurve::ParameterAt(double U) const
{
    const double Total = GetLength();
    if (!(Total > 0.0))
    {
        return 0.0;
    }
    const int32 N = Lengths.Num() - 1;
    const double Target = FMath::Clamp(U, 0.0, 1.0) * Total;
    int32 Low = 0;
    int32 High = N;
    while (High - Low > 1)
    {
        const int32 Mid = (Low + High) >> 1;
        if (Lengths[Mid] <= Target)
        {
            Low = Mid;
        }
        else
        {
            High = Mid;
        }
    }
    const double Span = Lengths[High] - Lengths[Low];
    const double Within = Span > 0.0 ? (Target - Lengths[Low]) / Span : 0.0;
    return FMath::Min(1.0, (Low + Within) / N);
}

void FLoomaPathCurve::SegmentPoints(int32 Index, FVector& P0, FVector& P1, FVector& P2, FVector& P3) const
{
    const int32 L = Points.Num();
    P1 = Points[Index % L];
    P2 = Points[(Index + 1) % L];
    // At the ends of an open curve the missing neighbour is the reflection of the next
    // point: P(−1) = 2·P(0) − P(1), P(n) = 2·P(n−1) − P(n−2). A closed curve wraps.
    //
    // three.js r169 reflects both ends into ONE scratch vector, so on a two-point open
    // path its P0 is overwritten by P3. The curve is the same straight line; only its
    // pace along t differs, which the length table all but cancels (well under a
    // millimetre on a 4 m path). The format's formula is followed here.
    P0 = (bClosed || Index > 0) ? Points[(Index - 1 + L) % L] : 2.0 * Points[0] - Points[1];
    P3 = (bClosed || Index + 2 < L) ? Points[(Index + 2) % L] : 2.0 * Points[L - 1] - Points[L - 2];
}

void FLoomaPathCurve::SegmentTangents(int32 Index, FVector& OutM1, FVector& OutM2) const
{
    FVector P0, P1, P2, P3;
    SegmentPoints(Index, P0, P1, P2, P3);
    // Centripetal: each knot interval is the square root of the chord's length.
    double D0 = FMath::Pow(FVector::DistSquared(P0, P1), 0.25);
    double D1 = FMath::Pow(FVector::DistSquared(P1, P2), 0.25);
    double D2 = FMath::Pow(FVector::DistSquared(P2, P3), 0.25);
    // Repeated points, in this order: three.js's own guard.
    if (D1 < KnotEpsilon) { D1 = 1.0; }
    if (D0 < KnotEpsilon) { D0 = D1; }
    if (D2 < KnotEpsilon) { D2 = D1; }
    OutM1 = ((P1 - P0) / D0 - (P2 - P0) / (D0 + D1) + (P2 - P1) / D1) * D1;
    OutM2 = ((P2 - P1) / D1 - (P3 - P1) / (D1 + D2) + (P3 - P2) / D2) * D1;
}

FVector FLoomaPathCurve::PointAtParameter(double T) const
{
    if (!IsCurve())
    {
        return Points.Num() == 1 ? Points[0] : FVector::ZeroVector;
    }
    const int32 Segments = NumSegments();
    const double P = Segments * FMath::Clamp(T, 0.0, 1.0);
    // The end of the last segment is t = 1 of that segment, not t = 0 of one past it.
    const int32 Index = FMath::Clamp(FMath::FloorToInt32(P), 0, Segments - 1);
    const double W = P - Index;

    FVector P0, P1, P2, P3;
    SegmentPoints(Index, P0, P1, P2, P3);
    FVector M1, M2;
    SegmentTangents(Index, M1, M2);
    // three.js's CubicPoly, coefficient for coefficient: the Hermite basis
    //   (2t³ − 3t² + 1)·P1 + (t³ − 2t² + t)·m1 + (−2t³ + 3t²)·P2 + (t³ − t²)·m2
    // collected by power of t.
    const FVector C0 = P1;
    const FVector C1 = M1;
    const FVector C2 = -3.0 * P1 + 3.0 * P2 - 2.0 * M1 - M2;
    const FVector C3 = 2.0 * P1 - 2.0 * P2 + M1 + M2;
    return C0 + C1 * W + C2 * (W * W) + C3 * (W * W * W);
}

FVector FLoomaPathCurve::TangentAtParameter(double T) const
{
    const double T1 = FMath::Max(0.0, T - TangentDelta);
    const double T2 = FMath::Min(1.0, T + TangentDelta);
    return (PointAtParameter(T2) - PointAtParameter(T1)).GetSafeNormal(0.0);
}

double LoomaPathTime(const FLoomaPathSpec& Path, double NowSeconds)
{
    // A path the hub has not stamped yet (an edit still on its way) waits at its
    // offset rather than guessing when it started.
    if (!Path.bPlaying || !Path.bStamped)
    {
        return Path.Offset;
    }
    return Path.Offset + FMath::Max(0.0, NowSeconds - Path.StartedAt);
}

double LoomaPathFoldedTime(const FLoomaPathSpec& Path, double Time)
{
    const double T = Path.Duration;
    switch (Path.Mode)
    {
    case ELoomaPathMode::Once:
        return FMath::Clamp(Time, 0.0, T);
    case ELoomaPathMode::PingPong:
    {
        const double K = FMath::Fmod(FMath::Fmod(Time, 2.0 * T) + 2.0 * T, 2.0 * T);
        return K <= T ? K : 2.0 * T - K;
    }
    case ELoomaPathMode::Loop:
    default:
        return FMath::Fmod(FMath::Fmod(Time, T) + T, T);
    }
}

double LoomaPathProgress(const FLoomaPathSpec& Path, double Time)
{
    const double U = LoomaPathFoldedTime(Path, Time) / Path.Duration;
    return Path.Ease == ELoomaPathEase::InOut ? U * U * (3.0 - 2.0 * U) : U;
}

void LoomaCarriageAt(const FLoomaPathSpec& Path, const FLoomaPathCurve& Curve, double U, FVector& OutPosition,
    FQuat& OutRotation)
{
    OutRotation = FQuat::Identity;
    if (!Curve.IsCurve())
    {
        OutPosition = Path.Points.Num() > 0 ? Path.Points[0] : FVector::ZeroVector;
        return;
    }
    const double T = Curve.ParameterAt(U);
    OutPosition = Curve.PointAtParameter(T);
    if (Path.Orient != ELoomaPathOrient::Path)
    {
        return;
    }
    const FVector Tangent = Curve.TangentAtParameter(T);
    if (Tangent.SizeSquared() < 1e-12)
    {
        return;
    }
    OutRotation = LoomaWireLookRotation(FVector::ZeroVector, Tangent, WireUp);
}

FQuat LoomaWireLookRotation(const FVector& Eye, const FVector& Target, const FVector& Up)
{
    // three.js's Matrix4.lookAt, line for line, including where it falls back.
    FVector Z = Eye - Target;
    if (Z.SizeSquared() == 0.0)
    {
        Z.Z = 1.0;
    }
    Z.Normalize(0.0);
    FVector X = FVector::CrossProduct(Up, Z);
    if (X.SizeSquared() == 0.0)
    {
        if (FMath::Abs(Up.Z) == 1.0)
        {
            Z.X += 1e-4;
        }
        else
        {
            Z.Z += 1e-4;
        }
        Z.Normalize(0.0);
        X = FVector::CrossProduct(Up, Z);
    }
    X.Normalize(0.0);
    const FVector Y = FVector::CrossProduct(Z, X);
    // The basis as rows: FQuat(FMatrix) reads the same components three.js's
    // setFromRotationMatrix reads from its columns, so the quaternion is the same.
    FQuat Out(FMatrix(X, Y, Z, FVector::ZeroVector));
    Out.Normalize();
    return Out;
}

bool LoomaAimRotation(const FVector& Eye, const FVector& Target, ELoomaLookAtAxis Axis, FQuat& OutRotation)
{
    if (FVector::DistSquared(Eye, Target) < 1e-10)
    {
        return false;
    }
    OutRotation = LoomaWireLookRotation(Eye, Target, WireUp);
    if (Axis == ELoomaLookAtAxis::MinusY)
    {
        // Turns the node's −Y onto where −Z would point: +90° about X.
        OutRotation = OutRotation * FQuat(FVector(1.0, 0.0, 0.0), UE_DOUBLE_HALF_PI);
    }
    return true;
}
