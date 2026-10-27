#include "LoomaCameraComponent.h"
#include "LoomaDrivers.h"
#include "LoomaHubClock.h"
#include "LoomaPostProcess.h"
#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaWireConvert.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

/**
 * The `camera`, `path`, `lookAt` and `postProcess` components (looma-xr-asset-demo
 * docs/scene-format.md, "Cameras, paths, aims and looks").
 *
 * The curve, time and aim numbers below are the web client's own: printed by its
 * frontend/src/scene/path/curve.js and frontend/src/scene/lookAt.js, on three.js
 * r169, from PR #298. Matching them is the point of fixing the length table in the
 * format, so a camera on a rail stands in the same place on every client.
 */
namespace LoomaCameraTests
{
TArray<TSharedPtr<FJsonValue>> ParseJsonArray(const FString& Json)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Values);
    return Values;
}

FLoomaNodeComponents Parse(const FString& Json)
{
    const TArray<TSharedPtr<FJsonValue>> Values = ParseJsonArray(Json);
    return LoomaParseComponents(&Values);
}

/** One web sample: at distance fraction U the curve's parameter is T, at Point, facing Facing. */
struct FWebSample
{
    double U;
    double T;
    FVector Point;
    FQuat Facing;
};

struct FWebCurve
{
    const TCHAR* Name;
    TArray<FVector> Points;
    bool bClosed;
    double Length;
    TArray<FWebSample> Samples;
};

TArray<FWebCurve> WebCurves()
{
    return {
        { TEXT("the format's example"),
            { FVector(0, 1.6, 0), FVector(2, 1.6, -3), FVector(5, 1.8, -3), FVector(7, 1.6, 0) }, false, 10.408325872,
            {
                { 0, 0, FVector(0, 1.6, 0), FQuat(-0.000004557, -0.28972728, -0.000001379, 0.957109243) },
                { 0.1, 0.091211339, FVector(0.510732226, 1.593773935, -0.906184792), FQuat(-0.004565571, -0.233101431, -0.001094402, 0.972441094) },
                { 0.25, 0.226852596, FVector(1.26172496, 1.583062852, -2.273673262), FQuat(0.000433322, -0.295197123, 0.000133882, 0.955436263) },
                { 0.5, 0.500233465, FVector(3.502352953, 1.711067945, -3.326710374), FQuat(0.028957091, -0.706751469, 0.028976631, 0.706274877) },
                { 0.75, 0.773646931, FVector(5.741530952, 1.76824935, -2.269325582), FQuat(0.007987738, 0.955267915, 0.025910103, -0.294496305) },
                { 0.9, 0.908966539, FVector(6.490304322, 1.666503083, -0.904345157), FQuat(0.008005669, 0.971886341, 0.033402953, -0.232931519) },
                { 1, 1, FVector(7, 1.6, 0), FQuat(0.008028345, 0.956741736, 0.02652151, -0.289615961) },
            } },
        { TEXT("a closed loop"),
            { FVector(0, 0, 0), FVector(4, 0, 0), FVector(4.5, 0, 3), FVector(0, 1, 3.5) }, true, 16.080062587,
            {
                { 0, 0, FVector(0, 0, 0), FQuat(-0.089671842, -0.390230127, -0.038220665, 0.915542784) },
                { 0.1, 0.101657169, FVector(1.513017227, -0.076809421, -0.44915039), FQuat(0.005266432, -0.682054115, 0.004912016, 0.731266245) },
                { 0.25, 0.236653584, FVector(3.868499775, -0.001447391, -0.092985512), FQuat(-0.004056647, 0.870041781, -0.007160451, -0.492909293) },
                { 0.5, 0.543811759, FVector(3.988001964, 0.137971111, 3.358431773), FQuat(0.064999454, 0.833571289, -0.100467404, 0.539296095) },
                { 0.75, 0.735311361, FVector(0.149385063, 0.998199494, 3.599199485), FQuat(0.023638176, 0.520634798, -0.014422149, 0.853330326) },
                { 0.9, 0.893480704, FVector(-0.456608352, 0.449874336, 1.444455374), FQuat(-0.149815234, -0.031660838, -0.004799938, 0.988195298) },
                { 1, 1, FVector(0, 0, 0), FQuat(-0.089905621, -0.389490853, -0.038235551, 0.91583399) },
            } },
        // Two points 10 cm apart among long chords: the case a uniform spline loops in.
        { TEXT("unevenly spaced points"),
            { FVector(0, 0, 0), FVector(0.1, 0, 0), FVector(3, 0, 1), FVector(3, 0, 5), FVector(3.05, 0.2, 5) }, false, 7.516225837,
            {
                { 0, 0, FVector(0, 0, 0), FQuat(0, -0.707067734, 0, 0.707145826) },
                { 0.1, 0.32346725, FVector(0.743582252, 0, 0.098491641), FQuat(0, -0.764532784, 0, 0.644584845) },
                { 0.25, 0.399854334, FVector(1.842027432, 0, 0.349754671), FQuat(0, -0.803752919, 0, 0.59496323) },
                { 0.5, 0.537153297, FVector(3.186808328, -0.013500572, 1.491609923), FQuat(0.001949604, 0.995109318, 0.020062496, -0.096701296) },
                { 0.75, 0.632069303, FVector(3.18390792, -0.094519443, 3.363148787), FQuat(-0.00077841, 0.998579791, 0.015226806, 0.051048412) },
                { 0.9, 0.693827121, FVector(3.044862002, -0.096971985, 4.481886384), FQuat(0.001627727, 0.997529324, -0.024696187, 0.065747222) },
                { 1, 1, FVector(3.05, 0.2, 5), FQuat(0.435454301, -0.556970944, 0.434869677, 0.557719717) },
            } },
        { TEXT("the smallest closed curve"),
            { FVector(0, 0, 0), FVector(2, 0, 0), FVector(1, 0, -2) }, true, 6.77486322,
            {
                { 0, 0, FVector(0, 0, 0), FQuat(0, 0.961781382, 0, -0.273818503) },
                { 0.1, 0.119926169, FVector(0.627067602, 0, 0.211765539), FQuat(0, -0.741860816, 0, 0.670553898) },
                { 0.25, 0.250730988, FVector(1.638999189, 0, 0.171369181), FQuat(0, -0.628879581, 0, 0.777502716) },
                { 0.5, 0.510616363, FVector(1.654744686, 0, -1.208730455), FQuat(0, 0.242167719, 0, 0.970234402) },
                { 0.75, 0.776799166, FVector(0.55259127, 0, -1.560350206), FQuat(0, 0.957965346, 0, 0.286883942) },
                { 0.9, 0.889742043, FVector(0.093628876, 0, -0.655716998), FQuat(0, 0.984136277, 0, 0.177414173) },
                { 1, 1, FVector(0, 0, 0), FQuat(0, 0.962112216, 0, -0.272653781) },
            } },
    };
}

/**
 * The same rotation to within `Tolerance` radians, whichever sign the quaternion was
 * written with. Not FQuat::AngularDistance, whose acos turns the rounding of a
 * nine-digit reference into a few hundred-thousandths of a radian.
 */
bool SameRotation(const FQuat& A, const FQuat& B, double Tolerance)
{
    const FQuat D = A.GetNormalized().Inverse() * B.GetNormalized();
    const double Angle = 2.0 * FMath::Atan2(FVector(D.X, D.Y, D.Z).Size(), FMath::Abs(D.W));
    return Angle < Tolerance;
}

FLoomaPathSpec PathOf(const TArray<FVector>& Points, bool bClosed, ELoomaPathOrient Orient)
{
    FLoomaPathSpec Path;
    Path.Points = Points;
    Path.bClosed = bClosed;
    Path.Orient = Orient;
    return Path;
}

/** The format's polyline length, sampled `PerSegment` times per segment. */
double PolylineLength(const FLoomaPathCurve& Curve, int32 PerSegment)
{
    const int32 N = Curve.NumSegments() * PerSegment;
    double Length = 0.0;
    FVector Previous = Curve.PointAtParameter(0.0);
    for (int32 J = 1; J <= N; ++J)
    {
        const FVector Next = Curve.PointAtParameter(static_cast<double>(J) / N);
        Length += FVector::Distance(Previous, Next);
        Previous = Next;
    }
    return Length;
}
} // namespace LoomaCameraTests

using namespace LoomaCameraTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaPathCurveTest, "Looma.Cameras.PathCurve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaPathCurveTest::RunTest(const FString& Parameters)
{
    for (const FWebCurve& Web : WebCurves())
    {
        const FLoomaPathCurve Curve(Web.Points, Web.bClosed);
        const FLoomaPathSpec Path = PathOf(Web.Points, Web.bClosed, ELoomaPathOrient::Path);
        const FString Name = Web.Name;

        // The format's table, 64 samples a segment, and not some other: Unreal's own
        // spline measures with 10 a segment, which lands measurably apart.
        TestEqual(Name + TEXT(": length is the web's"), Curve.GetLength(), Web.Length, 1e-6);
        TestTrue(Name + TEXT(": and a coarser table would not give it"),
            FMath::Abs(PolylineLength(Curve, 10) - Web.Length) > 1e-4);
        TestEqual(Name + TEXT(": the table is 64 samples a segment"),
            PolylineLength(Curve, LoomaPathSamplesPerSegment), Curve.GetLength(), 1e-12);

        // The curve passes through every point it is given.
        for (int32 I = 0; I < Web.Points.Num(); ++I)
        {
            const FVector At = Curve.PointAtParameter(static_cast<double>(I) / Curve.NumSegments());
            TestTrue(FString::Printf(TEXT("%s: passes through point %d"), *Name, I), At.Equals(Web.Points[I], 1e-9));
        }

        for (const FWebSample& Sample : Web.Samples)
        {
            const FString At = FString::Printf(TEXT("%s at u = %.2f"), *Name, Sample.U);
            TestEqual(At + TEXT(": parameter"), Curve.ParameterAt(Sample.U), Sample.T, 1e-6);
            TestTrue(At + FString::Printf(TEXT(": point (%s, web %s)"), *Curve.PointAt(Sample.U).ToString(),
                *Sample.Point.ToString()), Curve.PointAt(Sample.U).Equals(Sample.Point, 1e-6));

            FVector Position;
            FQuat Facing;
            LoomaCarriageAt(Path, Curve, Sample.U, Position, Facing);
            TestTrue(At + TEXT(": the carriage stands on the curve"), Position.Equals(Sample.Point, 1e-6));
            TestTrue(At + FString::Printf(TEXT(": facing along the path (%s, web %s)"), *Facing.ToString(),
                *Sample.Facing.ToString()), SameRotation(Facing, Sample.Facing, 1e-6));
            // Facing along the path is its −Z along the tangent.
            TestTrue(At + TEXT(": −Z is the tangent"),
                Facing.RotateVector(FVector(0, 0, -1)).Equals(Curve.TangentAt(Sample.U), 1e-6));
        }

        // Each segment is the Bezier with handles m1/3 and −m2/3, which is how a spline
        // that is not this one (Unreal's CurveCustomTangent) draws it exactly.
        for (int32 Segment = 0; Segment < Curve.NumSegments(); ++Segment)
        {
            FVector M1, M2;
            Curve.SegmentTangents(Segment, M1, M2);
            const FVector B0 = Curve.PointAtParameter(static_cast<double>(Segment) / Curve.NumSegments());
            const FVector B3 = Curve.PointAtParameter(static_cast<double>(Segment + 1) / Curve.NumSegments());
            const FVector B1 = B0 + M1 / 3.0;
            const FVector B2 = B3 - M2 / 3.0;
            for (const double W : { 0.2, 0.5, 0.8 })
            {
                const double V = 1.0 - W;
                const FVector Bezier = V * V * V * B0 + 3.0 * V * V * W * B1 + 3.0 * V * W * W * B2 + W * W * W * B3;
                const FVector Curved = Curve.PointAtParameter((Segment + W) / Curve.NumSegments());
                TestTrue(FString::Printf(TEXT("%s: segment %d is its Bezier at %.1f"), *Name, Segment, W),
                    Bezier.Equals(Curved, 1e-9));
            }
        }
    }

    // Two points, open. The format reflects both ends, which makes it a straight line
    // at an even pace. three.js r169 reflects both into one scratch vector, so its pace
    // differs; the length table all but cancels that, and these are its numbers.
    {
        const FLoomaPathCurve Two({ FVector(1, 0, 0), FVector(1, 0, -4) }, false);
        TestEqual(TEXT("two points: four metres"), Two.GetLength(), 4.0, 1e-9);
        const double Web[][2] = { { 0.1, -0.399770444 }, { 0.25, -0.999850518 }, { 0.5, -1.999968116 },
            { 0.75, -3.00007534 }, { 0.9, -3.600062061 } };
        for (const auto& Sample : Web)
        {
            const FVector At = Two.PointAt(Sample[0]);
            TestTrue(FString::Printf(TEXT("two points at u = %.2f: on the line at the format's pace"), Sample[0]),
                At.Equals(FVector(1, 0, -4.0 * Sample[0]), 1e-9));
            TestTrue(FString::Printf(TEXT("two points at u = %.2f: within a millimetre of the web"), Sample[0]),
                FMath::Abs(At.Z - Sample[1]) < 1e-3);
        }
        FVector Position;
        FQuat Facing;
        LoomaCarriageAt(PathOf({ FVector(1, 0, 0), FVector(1, 0, -4) }, false, ELoomaPathOrient::Path), Two, 0.5,
            Position, Facing);
        TestTrue(TEXT("two points along −Z: facing is the identity"), SameRotation(Facing, FQuat::Identity, 1e-9));
    }

    // Closed needs three points: two are drawn open.
    TestFalse(TEXT("closed with two points is open"), FLoomaPathCurve({ FVector(0), FVector(1, 0, 0) }, true).IsClosed());

    // Fewer than two points is not a curve: the carriage stands on the point, or the origin.
    {
        FVector Position;
        FQuat Facing;
        const FLoomaPathSpec One = PathOf({ FVector(1, 2, 3) }, false, ELoomaPathOrient::Path);
        LoomaCarriageAt(One, FLoomaPathCurve(One.Points, false), 0.7, Position, Facing);
        TestEqual(TEXT("one point: the carriage stands on it"), Position, FVector(1, 2, 3));
        TestTrue(TEXT("one point: and does not turn"), Facing.Equals(FQuat::Identity));
        const FLoomaPathSpec None = PathOf({}, false, ELoomaPathOrient::Path);
        LoomaCarriageAt(None, FLoomaPathCurve(None.Points, false), 0.7, Position, Facing);
        TestEqual(TEXT("no points: the carriage stands on the node's origin"), Position, FVector::ZeroVector);
    }

    // Fixed keeps the node's own axes.
    {
        const FWebCurve Spec = WebCurves()[0];
        FVector Position;
        FQuat Facing;
        LoomaCarriageAt(PathOf(Spec.Points, false, ELoomaPathOrient::Fixed), FLoomaPathCurve(Spec.Points, false), 0.5,
            Position, Facing);
        TestTrue(TEXT("orient fixed: no turn"), Facing.Equals(FQuat::Identity));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaPathTimeTest, "Looma.Cameras.PathTime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaPathTimeTest::RunTest(const FString& Parameters)
{
    // The web's progressAt for a 4 s path, at these seconds of travel.
    const double Times[] = { -1, 0, 1, 3.5, 4, 5, 7.5, 8, 9 };
    struct FRow
    {
        ELoomaPathMode Mode;
        ELoomaPathEase Ease;
        double Progress[9];
    };
    const FRow Rows[] = {
        { ELoomaPathMode::Loop, ELoomaPathEase::Linear, { 0.75, 0, 0.25, 0.875, 0, 0.25, 0.875, 0, 0.25 } },
        { ELoomaPathMode::Loop, ELoomaPathEase::InOut,
            { 0.84375, 0, 0.15625, 0.95703125, 0, 0.15625, 0.95703125, 0, 0.15625 } },
        { ELoomaPathMode::PingPong, ELoomaPathEase::Linear, { 0.25, 0, 0.25, 0.875, 1, 0.75, 0.125, 0, 0.25 } },
        { ELoomaPathMode::PingPong, ELoomaPathEase::InOut,
            { 0.15625, 0, 0.15625, 0.95703125, 1, 0.84375, 0.04296875, 0, 0.15625 } },
        { ELoomaPathMode::Once, ELoomaPathEase::Linear, { 0, 0, 0.25, 0.875, 1, 1, 1, 1, 1 } },
        { ELoomaPathMode::Once, ELoomaPathEase::InOut, { 0, 0, 0.15625, 0.95703125, 1, 1, 1, 1, 1 } },
    };
    for (const FRow& Row : Rows)
    {
        FLoomaPathSpec Path;
        Path.Duration = 4.0f;
        Path.Mode = Row.Mode;
        Path.Ease = Row.Ease;
        for (int32 I = 0; I < static_cast<int32>(UE_ARRAY_COUNT(Times)); ++I)
        {
            TestEqual(FString::Printf(TEXT("mode %d ease %d at %.1f s"), static_cast<int32>(Row.Mode),
                static_cast<int32>(Row.Ease), Times[I]), LoomaPathProgress(Path, Times[I]), Row.Progress[I], 1e-9);
        }
    }

    // On the hub's clock: offset + (now − startedAt) while playing, the offset otherwise.
    FLoomaPathSpec Path;
    Path.bStamped = true;
    Path.StartedAt = 100.0;
    Path.Offset = 2.0;
    TestEqual(TEXT("playing"), LoomaPathTime(Path, 103.5), 5.5);
    TestEqual(TEXT("a clock behind the stamp does not run time backwards"), LoomaPathTime(Path, 99.0), 2.0);
    Path.bPlaying = false;
    TestEqual(TEXT("paused: held at its offset"), LoomaPathTime(Path, 103.5), 2.0);
    Path.bPlaying = true;
    Path.bStamped = false;
    TestEqual(TEXT("not stamped by the hub yet: waits at its offset"), LoomaPathTime(Path, 103.5), 2.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaAimTest, "Looma.Cameras.Aim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaAimTest::RunTest(const FString& Parameters)
{
    // The web's aimQuaternion, eye, target and axis.
    struct FCase
    {
        const TCHAR* Name;
        FVector Eye;
        FVector Target;
        ELoomaLookAtAxis Axis;
        FQuat Web;
    };
    const FCase Cases[] = {
        { TEXT("a camera framing a performer"), FVector(0, 1.6, 5), FVector(2, 1, 0), ELoomaLookAtAxis::MinusZ,
            FQuat(-0.054450786, -0.188816559, -0.010486263, 0.98044554) },
        { TEXT("a follow-spot's beam"), FVector(0, 3, 0), FVector(1, 0, -1), ELoomaLookAtAxis::MinusY,
            FQuat(0.201848085, -0.37343845, 0.083608215, 0.90156017) },
        { TEXT("straight up, three.js's fallback"), FVector(1, 1, 1), FVector(1, 5, 1), ELoomaLookAtAxis::MinusZ,
            FQuat(0.707071425, 0, 0, 0.707142136) },
        { TEXT("a beam straight down"), FVector(0, 0, 0), FVector(0, -2, 0), ELoomaLookAtAxis::MinusY,
            FQuat(0.00005, 0, 0, 0.999999999) },
    };
    for (const FCase& Case : Cases)
    {
        FQuat Aim;
        TestTrue(FString(Case.Name) + TEXT(": aims"), LoomaAimRotation(Case.Eye, Case.Target, Case.Axis, Aim));
        TestTrue(FString::Printf(TEXT("%s: the web's turn (%s, web %s)"), Case.Name, *Aim.ToString(),
            *Case.Web.ToString()), SameRotation(Aim, Case.Web, 1e-5));
        const FVector Axis = Case.Axis == ELoomaLookAtAxis::MinusY ? FVector(0, -1, 0) : FVector(0, 0, -1);
        TestTrue(FString(Case.Name) + TEXT(": its axis points at the target"),
            Aim.RotateVector(Axis).Equals((Case.Target - Case.Eye).GetSafeNormal(), 1e-3));
    }
    FQuat Aim = FQuat::Identity;
    TestFalse(TEXT("no direction when the target is where the node is"),
        LoomaAimRotation(FVector(1, 2, 3), FVector(1, 2, 3), ELoomaLookAtAxis::MinusZ, Aim));

    // The wire's rotations land on Unreal's axes through LoomaWireConvert, products kept.
    const FQuat A(FVector(0.3, 0.8, -0.2).GetSafeNormal(), 0.7);
    const FQuat B(FVector(-0.5, 0.1, 0.9).GetSafeNormal(), -1.3);
    TestTrue(TEXT("a product converts as the product of the conversions"),
        SameRotation(LoomaWireQuatToUe(A * B), LoomaWireQuatToUe(A) * LoomaWireQuatToUe(B), 1e-9));
    const FVector P(0.4, -1.2, 2.5);
    TestTrue(TEXT("a rotated point converts as the converted rotation of the converted point"),
        LoomaWirePointToUe(A.RotateVector(P)).Equals(LoomaWireQuatToUe(A).RotateVector(LoomaWirePointToUe(P)), 1e-9));
    TestTrue(TEXT("a point round-trips"), LoomaUePointToWire(LoomaWirePointToUe(P)).Equals(P, 1e-12));
    TestTrue(TEXT("a rotation round-trips"), LoomaUeQuatToWire(LoomaWireQuatToUe(A)).Equals(A, 1e-12));
    // The node's −Z, which a camera looks down, is Unreal's +X, where a camera looks.
    TestTrue(TEXT("wire −Z is UE +X"), LoomaWirePointToUe(FVector(0, 0, -1)).Equals(FVector(100, 0, 0)));
    TestTrue(TEXT("wire +Y is UE +Z"), LoomaWirePointToUe(FVector(0, 1, 0)).Equals(FVector(0, 0, 100)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaCameraParseTest, "Looma.Cameras.Parse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaCameraParseTest::RunTest(const FString& Parameters)
{
    // --- camera -------------------------------------------------------------------
    {
        const FLoomaNodeComponents Bare = Parse(TEXT(R"([{"type":"camera"}])"));
        TestTrue(TEXT("camera: parsed"), Bare.bHasCamera && Bare.bCarriesCamera);
        TestEqual(TEXT("camera: perspective by default"), Bare.Camera.Projection, ELoomaCameraProjection::Perspective);
        TestEqual(TEXT("camera: fov 50"), Bare.Camera.Fov, 50.0f);
        TestEqual(TEXT("camera: orthoSize 5"), Bare.Camera.OrthoSize, 5.0f);
        TestEqual(TEXT("camera: near 0.1"), Bare.Camera.Near, 0.1f);
        TestEqual(TEXT("camera: far 1000"), Bare.Camera.Far, 1000.0f);
        TestFalse(TEXT("camera: no aspect, the screen's"), Bare.Camera.bHasAspect);
        TestFalse(TEXT("camera: not the scene's"), Bare.Camera.bPrimary);
        TestFalse(TEXT("camera: draws nothing, but is not an empty object"), Bare.IsEmpty());

        const FLoomaNodeComponents Full = Parse(TEXT(R"([{"type":"camera","projection":"orthographic","fov":40,
            "orthoSize":2.5,"near":0.5,"far":80,"aspect":2.39,"primary":true}])"));
        TestEqual(TEXT("camera: orthographic"), Full.Camera.Projection, ELoomaCameraProjection::Orthographic);
        TestEqual(TEXT("camera: fov"), Full.Camera.Fov, 40.0f);
        TestEqual(TEXT("camera: orthoSize"), Full.Camera.OrthoSize, 2.5f);
        TestEqual(TEXT("camera: near"), Full.Camera.Near, 0.5f);
        TestEqual(TEXT("camera: far"), Full.Camera.Far, 80.0f);
        TestTrue(TEXT("camera: aspect"), Full.Camera.bHasAspect && FMath::IsNearlyEqual(Full.Camera.Aspect, 2.39f));
        TestTrue(TEXT("camera: primary"), Full.Camera.bPrimary);

        // The hub's repairs, made again here: the wire is repaired, never rejected.
        const FLoomaNodeComponents Wild = Parse(TEXT(R"([{"type":"camera","projection":"fisheye","fov":200,
            "orthoSize":0,"near":0,"far":0,"aspect":40,"primary":"true"}])"));
        TestEqual(TEXT("camera: an unknown projection is perspective"), Wild.Camera.Projection,
            ELoomaCameraProjection::Perspective);
        TestEqual(TEXT("camera: fov at most 170"), Wild.Camera.Fov, 170.0f);
        TestEqual(TEXT("camera: orthoSize at least 0.001"), Wild.Camera.OrthoSize, 0.001f);
        TestEqual(TEXT("camera: near at least 0.001"), Wild.Camera.Near, 0.001f);
        TestEqual(TEXT("camera: far beyond near"), Wild.Camera.Far, 0.001f * 1.001f);
        TestEqual(TEXT("camera: aspect at most 10"), Wild.Camera.Aspect, 10.0f);
        TestFalse(TEXT("camera: only a literal true is primary"), Wild.Camera.bPrimary);
        TestEqual(TEXT("camera: fov at least 1"), Parse(TEXT(R"([{"type":"camera","fov":0}])")).Camera.Fov, 1.0f);
        TestEqual(TEXT("camera: a near plane past 1000 m pushes the default far out"),
            Parse(TEXT(R"([{"type":"camera","near":2000}])")).Camera.Far, 2000.0f * 1.001f);
        TestFalse(TEXT("camera: aspect null is the screen's"),
            Parse(TEXT(R"([{"type":"camera","aspect":null}])")).Camera.bHasAspect);
        TestEqual(TEXT("camera: a number nobody can read keeps the default"),
            Parse(TEXT(R"([{"type":"camera","fov":"wide"}])")).Camera.Fov, 50.0f);

        const FLoomaNodeComponents Off = Parse(TEXT(R"([{"type":"camera","primary":true,"enabled":false},
            {"type":"postProcess","exposure":1}])"));
        TestFalse(TEXT("camera off: sees nothing"), Off.bHasCamera);
        TestTrue(TEXT("camera off: still claims the node's postProcess"), Off.bCarriesCamera && Off.bHasPostProcess);
    }

    // --- path ---------------------------------------------------------------------
    {
        const FLoomaNodeComponents Bare = Parse(TEXT(R"([{"type":"path"}])"));
        TestTrue(TEXT("path: parsed"), Bare.bHasPath);
        TestEqual(TEXT("path: no points"), Bare.Path.Points.Num(), 0);
        TestFalse(TEXT("path: open"), Bare.Path.bClosed);
        TestEqual(TEXT("path: 10 s"), Bare.Path.Duration, 10.0f);
        TestTrue(TEXT("path: playing"), Bare.Path.bPlaying);
        TestEqual(TEXT("path: loop"), Bare.Path.Mode, ELoomaPathMode::Loop);
        TestEqual(TEXT("path: linear"), Bare.Path.Ease, ELoomaPathEase::Linear);
        TestEqual(TEXT("path: fixed"), Bare.Path.Orient, ELoomaPathOrient::Fixed);
        TestFalse(TEXT("path: not stamped"), Bare.Path.bStamped);
        TestEqual(TEXT("path: offset 0"), Bare.Path.Offset, 0.0);

        const FLoomaNodeComponents Full = Parse(TEXT(R"([{"type":"path","points":[[0,1.6,0],[2,1.6,-3],[5,1.8,-3],[7,1.6,0]],
            "closed":true,"duration":12,"playing":false,"mode":"pingpong","ease":"inOut","orient":"path",
            "startedAt":1761200000.5,"offset":3.25}])"));
        TestEqual(TEXT("path: four points"), Full.Path.Points.Num(), 4);
        TestEqual(TEXT("path: in wire metres and axes"), Full.Path.Points[1], FVector(2, 1.6, -3));
        TestTrue(TEXT("path: closed"), Full.Path.bClosed);
        TestEqual(TEXT("path: duration"), Full.Path.Duration, 12.0f);
        TestFalse(TEXT("path: paused"), Full.Path.bPlaying);
        TestEqual(TEXT("path: pingpong"), Full.Path.Mode, ELoomaPathMode::PingPong);
        TestEqual(TEXT("path: inOut"), Full.Path.Ease, ELoomaPathEase::InOut);
        TestEqual(TEXT("path: along the path"), Full.Path.Orient, ELoomaPathOrient::Path);
        TestTrue(TEXT("path: the hub's stamp, to the half second"),
            Full.Path.bStamped && Full.Path.StartedAt == 1761200000.5);
        TestEqual(TEXT("path: the hub's offset"), Full.Path.Offset, 3.25);

        const FLoomaNodeComponents Wild = Parse(TEXT(R"([{"type":"path","points":[[1,2,3],[0,0],"x",[1,2,"3"],[4,5,6,7],[7,8,9]],
            "closed":"yes","duration":0,"playing":0,"mode":"bounce","ease":"cubic","orient":"sideways","startedAt":"now"}])"));
        TestEqual(TEXT("path: a point that is not three numbers is dropped, not zeroed"), Wild.Path.Points.Num(), 2);
        TestEqual(TEXT("path: the next good point follows"), Wild.Path.Points[1], FVector(7, 8, 9));
        TestFalse(TEXT("path: only a literal true closes it"), Wild.Path.bClosed);
        TestEqual(TEXT("path: duration at least 0.1 s"), Wild.Path.Duration, 0.1f);
        TestTrue(TEXT("path: only a literal false pauses it"), Wild.Path.bPlaying);
        TestEqual(TEXT("path: an unknown mode is loop"), Wild.Path.Mode, ELoomaPathMode::Loop);
        TestEqual(TEXT("path: an unknown ease is linear"), Wild.Path.Ease, ELoomaPathEase::Linear);
        TestEqual(TEXT("path: an unknown orient is fixed"), Wild.Path.Orient, ELoomaPathOrient::Fixed);
        TestFalse(TEXT("path: a stamp nobody can read is no stamp"), Wild.Path.bStamped);
        TestEqual(TEXT("path: at most a day"),
            Parse(TEXT(R"([{"type":"path","duration":1e9}])")).Path.Duration, 86400.0f);

        FString Many = TEXT("[{\"type\":\"path\",\"points\":[");
        for (int32 I = 0; I < 300; ++I)
        {
            Many += FString::Printf(TEXT("%s[%d,0,0]"), I ? TEXT(",") : TEXT(""), I);
        }
        Many += TEXT("]}]");
        TestEqual(TEXT("path: at most 256 points"), Parse(Many).Path.Points.Num(), 256);
    }

    // --- lookAt -------------------------------------------------------------------
    {
        const FLoomaNodeComponents Full = Parse(TEXT(R"([{"type":"lookAt","target":"b2","axis":"-y","offset":[0,1.4,0]}])"));
        TestTrue(TEXT("lookAt: parsed"), Full.bHasLookAt);
        TestEqual(TEXT("lookAt: target"), Full.LookAt.Target, FString(TEXT("b2")));
        TestEqual(TEXT("lookAt: -y"), Full.LookAt.Axis, ELoomaLookAtAxis::MinusY);
        TestEqual(TEXT("lookAt: offset, wire metres"), Full.LookAt.Offset, FVector(0, 1.4, 0));
        const FLoomaNodeComponents Wild = Parse(TEXT(R"([{"type":"lookAt","target":null,"axis":"+x","offset":[1,2]}])"));
        TestTrue(TEXT("lookAt: null aims nothing"), Wild.LookAt.Target.IsEmpty());
        TestEqual(TEXT("lookAt: an unknown axis is -z"), Wild.LookAt.Axis, ELoomaLookAtAxis::MinusZ);
        TestEqual(TEXT("lookAt: an offset that is not three numbers is the origin"), Wild.LookAt.Offset, FVector::ZeroVector);
    }

    // --- postProcess --------------------------------------------------------------
    {
        const FLoomaNodeComponents Bare = Parse(TEXT(R"([{"type":"postProcess"}])"));
        TestTrue(TEXT("postProcess: parsed"), Bare.bHasPostProcess);
        TestTrue(TEXT("postProcess: an absent setting is absent"), Bare.PostProcess.IsEmpty());

        const FLoomaNodeComponents Full = Parse(TEXT(R"([{"type":"postProcess","priority":3,"exposure":0.5,
            "toneMapping":"agx","contrast":0.15,"saturation":-0.2,"colorFilter":"#ff8000","bloomIntensity":0.8,
            "bloomThreshold":0.9,"vignette":0.35,"depthOfField":true,"focusDistance":3,"fStop":1.4,
            "chromaticAberration":0.2,"grain":0.1}])"));
        const FLoomaPostProcessSpec& Look = Full.PostProcess;
        TestEqual(TEXT("postProcess: priority"), Look.Priority, 3.0f);
        TestTrue(TEXT("postProcess: exposure"), Look.bHasExposure && Look.Exposure == 0.5f);
        TestTrue(TEXT("postProcess: toneMapping"), Look.bHasToneMapping && Look.ToneMapping == ELoomaToneMapping::Agx);
        TestTrue(TEXT("postProcess: contrast"), Look.bHasContrast && Look.Contrast == 0.15f);
        TestTrue(TEXT("postProcess: saturation"), Look.bHasSaturation && Look.Saturation == -0.2f);
        TestTrue(TEXT("postProcess: colorFilter, in linear light"), Look.bHasColorFilter
            && Look.ColorFilter.Equals(FLinearColor::FromSRGBColor(FColor(255, 128, 0))));
        TestTrue(TEXT("postProcess: bloomIntensity"), Look.bHasBloomIntensity && Look.BloomIntensity == 0.8f);
        TestTrue(TEXT("postProcess: bloomThreshold"), Look.bHasBloomThreshold && Look.BloomThreshold == 0.9f);
        TestTrue(TEXT("postProcess: vignette"), Look.bHasVignette && Look.Vignette == 0.35f);
        TestTrue(TEXT("postProcess: depthOfField"), Look.bHasDepthOfField && Look.bDepthOfField);
        TestTrue(TEXT("postProcess: focusDistance"), Look.bHasFocusDistance && Look.FocusDistance == 3.0f);
        TestTrue(TEXT("postProcess: fStop"), Look.bHasFStop && Look.FStop == 1.4f);
        TestTrue(TEXT("postProcess: chromaticAberration"), Look.bHasChromaticAberration && Look.ChromaticAberration == 0.2f);
        TestTrue(TEXT("postProcess: grain"), Look.bHasGrain && Look.Grain == 0.1f);

        // Clamped when it can be read, and ABSENT when it cannot: any value put in its
        // place would override whatever is underneath.
        const FLoomaPostProcessSpec Wild = Parse(TEXT(R"([{"type":"postProcess","exposure":99,"contrast":-5,
            "toneMapping":"filmic","colorFilter":"chartreuse","depthOfField":"yes","grain":"lots","fStop":0.1}])")).PostProcess;
        TestEqual(TEXT("postProcess: exposure at most 10"), Wild.Exposure, 10.0f);
        TestEqual(TEXT("postProcess: contrast at least -1"), Wild.Contrast, -1.0f);
        TestEqual(TEXT("postProcess: fStop at least 0.5"), Wild.FStop, 0.5f);
        TestFalse(TEXT("postProcess: an unknown tone curve is absent"), Wild.bHasToneMapping);
        TestFalse(TEXT("postProcess: a colour that is not one is absent"), Wild.bHasColorFilter);
        TestFalse(TEXT("postProcess: depth of field that is not a bool is absent"), Wild.bHasDepthOfField);
        TestFalse(TEXT("postProcess: a number nobody can read is absent"), Wild.bHasGrain);
    }

    // --- enabled: false on each -----------------------------------------------------
    {
        const FLoomaNodeComponents Off = Parse(TEXT(R"([{"type":"path","points":[[0,0,0],[1,0,0]],"enabled":false},
            {"type":"lookAt","target":"b2","enabled":false},{"type":"postProcess","exposure":1,"enabled":false}])"));
        TestFalse(TEXT("path off: carries nothing"), Off.bHasPath);
        TestFalse(TEXT("lookAt off: aims nothing"), Off.bHasLookAt);
        TestFalse(TEXT("postProcess off: applies nothing"), Off.bHasPostProcess);
        TestTrue(TEXT("none of them is unknown"), Parse(TEXT(R"([{"type":"camera"},{"type":"path"},{"type":"lookAt"},
            {"type":"postProcess"}])")).UnknownTypes.IsEmpty());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaCameraLensTest, "Looma.Cameras.Lens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaCameraLensTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("a square frame's horizontal angle is its vertical one"), LoomaHorizontalFov(50.0f, 1.0f), 50.0f, 1e-4f);
    const float Scope = LoomaHorizontalFov(40.0f, 2.39f);
    TestEqual(TEXT("40° across a 2.39:1 frame is 82° across"), Scope,
        static_cast<float>(FMath::RadiansToDegrees(2.0 * FMath::Atan(2.39 * FMath::Tan(FMath::DegreesToRadians(20.0))))),
        1e-4f);
    TestEqual(TEXT("which is about 82.03°"), Scope, 82.03f, 0.01f);

    TStrongObjectPtr<ULoomaCameraComponent> Camera(NewObject<ULoomaCameraComponent>());
    FMinimalViewInfo View;

    FLoomaCameraSpec Spec;
    Spec.Fov = 35.0f;
    Spec.Near = 0.25f;
    Camera->ApplySpec(Spec);
    TestTrue(TEXT("no aspect: perspective"), Camera->ProjectionMode == ECameraProjectionMode::Perspective);
    TestFalse(TEXT("no aspect: fills the screen"), static_cast<bool>(Camera->bConstrainAspectRatio));
    TestTrue(TEXT("no aspect: keeps its vertical angle on any screen"),
        Camera->bOverrideAspectRatioAxisConstraint
            && Camera->AspectRatioAxisConstraint == EAspectRatioAxisConstraint::AspectRatio_MaintainYFOV);
    TestEqual(TEXT("no aspect: FieldOfView is that vertical angle"), Camera->FieldOfView, 35.0f);
    Camera->GetCameraView(0.0f, View);
    TestEqual(TEXT("near: on the view, in cm"), View.PerspectiveNearClipPlane, 25.0f);
    TestEqual(TEXT("near: the view keeps the angle"), View.FOV, 35.0f);
    TestFalse(TEXT("the shot is not steered by a headset"), static_cast<bool>(Camera->bLockToHmd));

    Spec.bHasAspect = true;
    Spec.Aspect = 2.39f;
    Spec.Fov = 40.0f;
    Camera->ApplySpec(Spec);
    TestTrue(TEXT("aspect: letterboxed"), static_cast<bool>(Camera->bConstrainAspectRatio));
    TestEqual(TEXT("aspect: the frame"), Camera->AspectRatio, 2.39f);
    TestEqual(TEXT("aspect: FieldOfView is the frame's horizontal angle"), Camera->FieldOfView, Scope);

    Spec.Projection = ELoomaCameraProjection::Orthographic;
    Spec.OrthoSize = 3.0f;
    Spec.Near = 0.5f;
    Spec.Far = 40.0f;
    Camera->ApplySpec(Spec);
    TestTrue(TEXT("orthographic"), Camera->ProjectionMode == ECameraProjectionMode::Orthographic);
    TestEqual(TEXT("orthographic with a frame: OrthoWidth is the full width, cm"), Camera->OrthoWidth, 600.0f * 2.39f, 1e-3f);
    TestFalse(TEXT("orthographic: its own clip planes"), Camera->bAutoCalculateOrthoPlanes);
    TestEqual(TEXT("orthographic: near, cm"), Camera->OrthoNearClipPlane, 50.0f);
    TestEqual(TEXT("orthographic: far, cm"), Camera->OrthoFarClipPlane, 4000.0f);
    Spec.bHasAspect = false;
    Camera->ApplySpec(Spec);
    // Under MaintainYFOV an orthographic OrthoWidth is the view's full height.
    TestEqual(TEXT("orthographic, no frame: OrthoWidth is the full height"), Camera->OrthoWidth, 600.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLookTest, "Looma.Cameras.Look",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLookTest::RunTest(const FString& Parameters)
{
    auto Volume = [](const TCHAR* Json) { return Parse(FString::Printf(TEXT("[%s]"), Json)).PostProcess; };

    // Globals stack by priority, lowest first, then document order; each overrides only
    // what it carries.
    TArray<FLoomaLookVolume> Globals;
    Globals.Add({ Volume(TEXT(R"({"type":"postProcess","priority":5,"exposure":2})")), 0 });
    Globals.Add({ Volume(TEXT(R"({"type":"postProcess","exposure":-1,"contrast":0.3,"vignette":0.2})")), 1 });
    Globals.Add({ Volume(TEXT(R"({"type":"postProcess","contrast":0.1})")), 2 });
    FLoomaPostProcessSpec Look;
    TestTrue(TEXT("a volume applies"), LoomaResolveLook(Globals, nullptr, Look));
    TestEqual(TEXT("the higher priority wins, whatever its place"), Look.Exposure, 2.0f);
    TestEqual(TEXT("equal priority: the later in the document wins"), Look.Contrast, 0.1f);
    TestTrue(TEXT("a setting only one volume carries survives the others"), Look.bHasVignette && Look.Vignette == 0.2f);
    TestFalse(TEXT("one nobody carries stays absent"), Look.bHasSaturation);

    const FLoomaPostProcessSpec Own = Volume(TEXT(R"({"type":"postProcess","priority":-100,"vignette":0.6})"));
    TestTrue(TEXT("a camera's own applies"), LoomaResolveLook(Globals, &Own, Look));
    TestEqual(TEXT("a camera's own goes on top of every global, its priority aside"), Look.Vignette, 0.6f);
    TestEqual(TEXT("and changes nothing it does not carry"), Look.Exposure, 2.0f);
    TestFalse(TEXT("no volume at all: nothing applies"), LoomaResolveLook({}, nullptr, Look));

    // The mapping onto FPostProcessSettings: an override for what the look carries,
    // and none for anything else.
    const FPostProcessSettings None = LoomaMakePostProcessSettings(FLoomaPostProcessSpec(), 1.5f);
    TestFalse(TEXT("nothing carried: no exposure override"), static_cast<bool>(None.bOverride_AutoExposureBias));
    TestFalse(TEXT("nothing carried: no tone override"), static_cast<bool>(None.bOverride_ToneCurveAmount));
    TestFalse(TEXT("nothing carried: no depth of field override"),
        static_cast<bool>(None.bOverride_DepthOfFieldFocalDistance));
    TestFalse(TEXT("nothing carried: no bloom override"), static_cast<bool>(None.bOverride_BloomIntensity));

    const FPostProcessSettings All = LoomaMakePostProcessSettings(Volume(TEXT(R"({"type":"postProcess",
        "exposure":0.5,"toneMapping":"none","contrast":0.15,"saturation":-0.2,"colorFilter":"#ffffff",
        "bloomIntensity":0.8,"bloomThreshold":0.9,"vignette":0.35,"depthOfField":true,"focusDistance":3,"fStop":1.4,
        "chromaticAberration":0.2,"grain":0.1})")), 2.39f);
    TestTrue(TEXT("exposure: manual metering"), All.bOverride_AutoExposureMethod && All.AutoExposureMethod == AEM_Manual);
    TestTrue(TEXT("exposure: physical camera exposure off"),
        All.bOverride_AutoExposureApplyPhysicalCameraExposure && !All.AutoExposureApplyPhysicalCameraExposure);
    TestTrue(TEXT("exposure: the stops are the bias"), All.bOverride_AutoExposureBias && All.AutoExposureBias == 0.5f);
    TestTrue(TEXT("toneMapping none: no tone curve"), All.bOverride_ToneCurveAmount && All.ToneCurveAmount == 0.0f);
    TestTrue(TEXT("contrast: 1 + contrast on every channel"),
        All.bOverride_ColorContrast && All.ColorContrast.Equals(FVector4(1.15f, 1.15f, 1.15f, 1.0f), 1e-6f));
    TestTrue(TEXT("saturation: 1 + saturation"),
        All.bOverride_ColorSaturation && All.ColorSaturation.Equals(FVector4(0.8f, 0.8f, 0.8f, 1.0f), 1e-6f));
    TestTrue(TEXT("colorFilter: the gain"), All.bOverride_ColorGain && All.ColorGain.Equals(FVector4(1, 1, 1, 1), 1e-6f));
    TestTrue(TEXT("bloomIntensity"), All.bOverride_BloomIntensity && All.BloomIntensity == 0.8f);
    TestTrue(TEXT("bloomThreshold"), All.bOverride_BloomThreshold && All.BloomThreshold == 0.9f);
    TestTrue(TEXT("vignette"), All.bOverride_VignetteIntensity && All.VignetteIntensity == 0.35f);
    TestTrue(TEXT("depth of field: focus, cm"),
        All.bOverride_DepthOfFieldFocalDistance && All.DepthOfFieldFocalDistance == 300.0f);
    TestTrue(TEXT("depth of field: f-stop"), All.bOverride_DepthOfFieldFstop && All.DepthOfFieldFstop == 1.4f);
    TestTrue(TEXT("depth of field: a sensor 24 mm high, as wide as the frame"),
        All.bOverride_DepthOfFieldSensorWidth && FMath::IsNearlyEqual(All.DepthOfFieldSensorWidth, 24.0f * 2.39f));
    TestTrue(TEXT("chromaticAberration: fringe"),
        All.bOverride_SceneFringeIntensity && FMath::IsNearlyEqual(All.SceneFringeIntensity, 1.0f));
    TestTrue(TEXT("grain"), All.bOverride_FilmGrainIntensity && All.FilmGrainIntensity == 0.1f);

    TestEqual(TEXT("toneMapping aces keeps Unreal's filmic curve"), LoomaMakePostProcessSettings(
        Volume(TEXT(R"({"type":"postProcess","toneMapping":"aces"})")), 1.5f).ToneCurveAmount, 1.0f);
    const FPostProcessSettings Off = LoomaMakePostProcessSettings(
        Volume(TEXT(R"({"type":"postProcess","depthOfField":false,"fStop":1.4})")), 1.5f);
    TestTrue(TEXT("depth of field off: focal distance 0"),
        Off.bOverride_DepthOfFieldFocalDistance && Off.DepthOfFieldFocalDistance == 0.0f);
    TestFalse(TEXT("depth of field off: no f-stop"), static_cast<bool>(Off.bOverride_DepthOfFieldFstop));
    const FPostProcessSettings Neutral = LoomaMakePostProcessSettings(
        Volume(TEXT(R"({"type":"postProcess","depthOfField":true})")), 1.5f);
    TestEqual(TEXT("depth of field on, no focus: 5 m"), Neutral.DepthOfFieldFocalDistance, 500.0f);
    TestEqual(TEXT("depth of field on, no f-stop: f/2.8"), Neutral.DepthOfFieldFstop, 2.8f);
    TestFalse(TEXT("focus without depth of field: left to the level"), static_cast<bool>(LoomaMakePostProcessSettings(
        Volume(TEXT(R"({"type":"postProcess","focusDistance":2})")), 1.5f).bOverride_DepthOfFieldFocalDistance));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaHubClockTest, "Looma.Cameras.HubClock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaHubClockTest::RunTest(const FString& Parameters)
{
    FLoomaHubClock Clock;
    TestFalse(TEXT("no answer: not synced"), Clock.IsSynced());
    TestEqual(TEXT("no answer: this machine's clock"), Clock.ServerNowMs(1000.0), 1000.0);

    // offset = server − (t0 + t3) / 2
    TestTrue(TEXT("an answer is kept"), Clock.AddSample(1000.0, 5050.0, 1100.0));
    TestEqual(TEXT("offset from the middle of the round trip"), Clock.GetOffsetMs(), 4000.0, 1e-9);
    TestEqual(TEXT("round trip"), Clock.GetRoundTripMs(), 100.0);
    Clock.AddSample(2000.0, 6015.0, 2020.0);
    TestEqual(TEXT("the shortest round trip wins"), Clock.GetOffsetMs(), 4005.0);
    Clock.AddSample(3000.0, 9000.0, 3300.0);
    TestEqual(TEXT("a slower answer does not replace it"), Clock.GetOffsetMs(), 4005.0);
    TestFalse(TEXT("a round trip over 5 s says nothing"), Clock.AddSample(0.0, 0.0, 6000.0));
    TestFalse(TEXT("nor does one that came back before it left"), Clock.AddSample(100.0, 0.0, 50.0));
    for (int32 I = 0; I < FLoomaHubClock::Keep; ++I)
    {
        Clock.AddSample(10000.0 + I, 20000.0 + I, 10200.0 + I);
    }
    TestEqual(TEXT("the best falls out after Keep newer answers"), Clock.GetRoundTripMs(), 200.0);
    TestEqual(TEXT("its offset with it"), Clock.GetOffsetMs(), 9900.0);
    Clock.Reset();
    TestFalse(TEXT("reset: not synced"), Clock.IsSynced());
    TestTrue(TEXT("the local clock is Unix milliseconds"),
        FMath::Abs(FLoomaHubClock::LocalMs() - (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds()) < 2000.0);
    return true;
}

/** Real scene frames into a subsystem, without restoring a session or opening a socket. */
struct FLoomaCameraTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};
    UWorld* World = nullptr;
    int32 NextOrder = 0;

    FLoomaCameraTestAccess()
    {
        const UWorld::InitializationValues Values = UWorld::InitializationValues()
            .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
    }

    ~FLoomaCameraTestAccess()
    {
        Sync->Tracked.Reset();
        World->DestroyWorld(false);
    }

    /** A node at `WirePosition`, after every node seeded before it in the document. */
    ALoomaSyncedActor* Seed(const TCHAR* Id, const FVector& WirePosition = FVector::ZeroVector)
    {
        ALoomaSyncedActor* Actor = World->SpawnActor<ALoomaSyncedActor>();
        Actor->Id = Id;
        Actor->DisplayName = Id;
        Actor->SetLocalTransform(FTransform(LoomaWirePointToUe(WirePosition)));
        FLoomaTrackedActor Entry;
        Entry.Actor = Actor;
        Entry.Order = NextOrder++;
        Sync->Tracked.Add(Id, Entry);
        return Actor;
    }

    void Receive(const FString& Json) { Sync->OnRawMessage(Json); }

    APostProcessVolume* LookVolume() const { return Sync->LookVolume.Get(); }

    void Patch(const TCHAR* Id, const TCHAR* Components)
    {
        Receive(FString::Printf(TEXT(R"({"type":"patch","nodes":[{"id":"%s","components":%s}]})"), Id, Components));
    }

    /** One frame of what the subsystem's Tick does for these components. */
    void Frame()
    {
        Sync->RefreshSkybox();
        Sync->RefreshCameras();
        Sync->TickDrivers();
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaCameraSceneTest, "Looma.Cameras.Scene",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaCameraSceneTest::RunTest(const FString& Parameters)
{
    FLoomaCameraTestAccess Access;
    const FWebCurve Spec = WebCurves()[0];

    // --- A rail with a rider ---------------------------------------------------------
    ALoomaSyncedActor* Rail = Access.Seed(TEXT("rail"));
    ALoomaSyncedActor* Rider = Access.Seed(TEXT("rider"));
    Access.Receive(TEXT(R"({"type":"reparent","nodes":[{"id":"rider","parent":"rail","t":{"p":[0,0,0]}}]})"));
    TestTrue(TEXT("a child rides its parent's carriage"),
        Rider->GetRootComponent()->GetAttachParent() == Rail->CarriageComponent.Get());
    TestEqual(TEXT("and its parent is still the node"), Rider->GetAttachParentActor(), static_cast<AActor*>(Rail));

    // Paused at 3 s of a 12 s loop: a quarter of the way along, whatever the clock says.
    Access.Patch(TEXT("rail"), TEXT(R"([{"type":"path","points":[[0,1.6,0],[2,1.6,-3],[5,1.8,-3],[7,1.6,0]],
        "duration":12,"playing":false,"offset":3,"startedAt":1761200000}])"));
    Access.Frame();
    TestTrue(FString::Printf(TEXT("the rider is where the web puts it (%s)"), *Rider->GetActorLocation().ToString()),
        Rider->GetActorLocation().Equals(LoomaWirePointToUe(Spec.Samples[2].Point), 1e-3));
    TestTrue(TEXT("the rider's own t is untouched"), Rider->GetLocalTransform().Equals(FTransform::Identity));
    TestTrue(TEXT("and so is the rail's"), Rail->GetLocalTransform().Equals(FTransform::Identity));

    // A rider half a metre above the carriage rides half a metre above the curve.
    Access.Receive(TEXT(R"({"type":"transform","transient":false,"nodes":[{"id":"rider","t":{"p":[0,0.5,0]}}]})"));
    TestTrue(TEXT("t is relative to the carriage"), Rider->GetActorLocation().Equals(
        LoomaWirePointToUe(Spec.Samples[2].Point + FVector(0, 0.5, 0)), 1e-3));

    // Facing along the path turns the carriage, and the rider with it.
    Access.Patch(TEXT("rail"), TEXT(R"([{"type":"path","points":[[0,1.6,0],[2,1.6,-3],[5,1.8,-3],[7,1.6,0]],
        "duration":12,"playing":false,"offset":3,"startedAt":1761200000,"orient":"path"}])"));
    Access.Frame();
    TestTrue(TEXT("orient path: the rider faces along the curve"), SameRotation(Rider->GetActorQuat(),
        LoomaWireQuatToUe(Spec.Samples[2].Facing), 1e-4));

    // Playing, on the hub's clock: a clock answer saying the hub is 1000 s ahead of this
    // machine, and a path stamped 3 s before the hub's now, is 3 s along.
    const double Local = FLoomaHubClock::LocalMs();
    Access.Receive(FString::Printf(TEXT(R"({"type":"clock","t0":%.3f,"server":%.3f})"), Local, Local + 1000000.0));
    TestTrue(TEXT("the hub's clock answers"), Access.Sync->GetHubClock().IsSynced());
    TestEqual(TEXT("and is 1000 s ahead"), Access.Sync->GetHubTime() - FLoomaHubClock::LocalMs() / 1000.0, 1000.0, 0.5);
    Access.Patch(TEXT("rail"), *FString::Printf(TEXT(R"([{"type":"path","points":[[0,1.6,0],[2,1.6,-3],[5,1.8,-3],[7,1.6,0]],
        "duration":12,"startedAt":%.3f,"offset":0}])"), Access.Sync->GetHubTime() - 3.0));
    Access.Frame();
    TestTrue(TEXT("playing: placed on the hub's clock"), Rider->GetActorLocation().Equals(
        LoomaWirePointToUe(Spec.Samples[2].Point + FVector(0, 0.5, 0)), 5.0));

    // Switched off, or taken away: the rider stands on the node again.
    Access.Patch(TEXT("rail"), TEXT(R"([{"type":"path","points":[[0,1.6,0],[2,1.6,-3]],"enabled":false}])"));
    Access.Frame();
    TestTrue(TEXT("path off: the rider stands relative to the node itself"),
        Rider->GetActorLocation().Equals(LoomaWirePointToUe(FVector(0, 0.5, 0)), 1e-3));
    TestEqual(TEXT("path off: the definition stays on the node"), Rail->WireComponents.Num(), 1);

    // --- A camera aiming at a performer ------------------------------------------------
    ALoomaSyncedActor* Performer = Access.Seed(TEXT("performer"), FVector(2, 1, 0));
    ALoomaSyncedActor* Cam = Access.Seed(TEXT("cam"), FVector(0, 1.6, 5));
    // The node's own rotation is replaced by the aim, not added to.
    Cam->SetLocalTransform(FTransform(FQuat(FVector::UpVector, 0.6), Cam->GetActorLocation()));
    Access.Patch(TEXT("cam"), TEXT(R"([{"type":"camera","fov":40,"aspect":2.39,"primary":true},
        {"type":"lookAt","target":"performer"}])"));
    Access.Frame();
    if (TestNotNull(TEXT("the camera is built"), Cam->CameraComponent.Get()))
    {
        TestTrue(TEXT("on the aim"), Cam->CameraComponent->GetAttachParent() == Cam->AimComponent.Get());
        TestTrue(TEXT("with no turn of its own"), Cam->CameraComponent->GetRelativeRotation().IsNearlyZero());
        const FQuat Web(-0.054450786, -0.188816559, -0.010486263, 0.98044554);
        TestTrue(FString::Printf(TEXT("aimed as the web aims it (%s)"), *Cam->CameraComponent->GetComponentQuat().ToString()),
            SameRotation(Cam->CameraComponent->GetComponentQuat(), LoomaWireQuatToUe(Web), 1e-4));
        TestTrue(TEXT("looking at the performer"), Cam->CameraComponent->GetForwardVector().Equals(
            (Performer->GetActorLocation() - Cam->GetActorLocation()).GetSafeNormal(), 1e-3));
        TestTrue(TEXT("its stored t keeps its own rotation"),
            Cam->GetLocalTransform().GetRotation().Equals(FQuat(FVector::UpVector, 0.6), 1e-9));
    }
    TestEqual(TEXT("the scene's camera"), Access.Sync->GetSceneCamera(), Cam);
    TestEqual(TEXT("the scene's cameras"), Access.Sync->GetCameras().Num(), 1);

    // An offset in the target's frame: its chest, not its feet.
    Access.Patch(TEXT("cam"), TEXT(R"([{"type":"camera","primary":true},{"type":"lookAt","target":"performer","offset":[0,1.4,0]}])"));
    Access.Frame();
    TestTrue(TEXT("aims at the offset"), Cam->CameraComponent->GetForwardVector().Equals((Performer->GetActorLocation()
        + FVector(0, 0, 140) - Cam->GetActorLocation()).GetSafeNormal(), 1e-3));

    // Nothing to aim at: the node shows its own rotation again.
    Access.Patch(TEXT("cam"), TEXT(R"([{"type":"camera","primary":true},{"type":"lookAt","target":"nobody"}])"));
    Access.Frame();
    TestTrue(TEXT("an id the scene does not hold aims nothing"), Cam->AimComponent->GetRelativeRotation().IsNearlyZero());
    ALoomaSyncedActor* Carried = Access.Seed(TEXT("carried"));
    Access.Receive(TEXT(R"({"type":"reparent","nodes":[{"id":"carried","parent":"cam","t":{"p":[0,0,-2]}}]})"));
    Access.Patch(TEXT("cam"), TEXT(R"([{"type":"camera","primary":true},{"type":"lookAt","target":"carried"}])"));
    Access.Frame();
    TestTrue(TEXT("a target the node carries aims nothing"), Cam->AimComponent->GetRelativeRotation().IsNearlyZero());
    Access.Patch(TEXT("cam"), TEXT(R"([{"type":"camera","primary":true},{"type":"lookAt","target":"performer","enabled":false}])"));
    Access.Frame();
    TestTrue(TEXT("lookAt off: aims nothing"), Cam->AimComponent->GetRelativeRotation().IsNearlyZero());
    TestNotNull(TEXT("the carried node keeps its place"), Carried->GetAttachParentActor());

    // A light's beam: its −Y. The light's own turn is inside the aim.
    ALoomaSyncedActor* Spot = Access.Seed(TEXT("spot"), FVector(0, 3, 0));
    Access.Patch(TEXT("spot"), TEXT(R"([{"type":"light","lightType":"spot"},{"type":"lookAt","target":"performer","axis":"-y"}])"));
    Access.Frame();
    if (TestNotNull(TEXT("the spot is built"), Spot->LightComponent.Get()))
    {
        TestTrue(TEXT("the beam points at the performer"), Spot->LightComponent->GetForwardVector().Equals(
            (Performer->GetActorLocation() - Spot->GetActorLocation()).GetSafeNormal(), 1e-3));
    }

    // --- Which camera is the scene's ---------------------------------------------------
    ALoomaSyncedActor* Second = Access.Seed(TEXT("second"));
    Access.Patch(TEXT("second"), TEXT(R"([{"type":"camera","primary":true}])"));
    Access.Frame();
    TestEqual(TEXT("two claim it: the first in document order wins"), Access.Sync->GetSceneCamera(), Cam);
    Access.Patch(TEXT("cam"), TEXT(R"([{"type":"camera","primary":true,"enabled":false}])"));
    Access.Frame();
    TestNull(TEXT("a camera switched off sees nothing"), Cam->CameraComponent.Get());
    TestEqual(TEXT("and is passed over"), Access.Sync->GetSceneCamera(), Second);
    Access.Patch(TEXT("second"), TEXT(R"([{"type":"camera"}])"));
    Access.Frame();
    TestNull(TEXT("nobody claims it: no scene camera"), Access.Sync->GetSceneCamera());
    TestEqual(TEXT("LookThrough resolves an id"), Access.Sync->ResolveCamera(TEXT("second")), Second);
    TestNull(TEXT("and the scene's camera to none"), Access.Sync->ResolveCamera(ULoomaSceneSyncSubsystem::SceneCameraName));
    TestNull(TEXT("and a camera switched off to none"), Access.Sync->ResolveCamera(TEXT("cam")));

    // --- Looks ---------------------------------------------------------------------------
    ALoomaSyncedActor* Grade = Access.Seed(TEXT("grade"));
    Access.Patch(TEXT("grade"), TEXT(R"([{"type":"postProcess","exposure":-1,"contrast":0.2}])"));
    Access.Patch(TEXT("second"), TEXT(R"([{"type":"camera"},{"type":"postProcess","exposure":1}])"));
    Access.Patch(TEXT("cam"), TEXT(R"([{"type":"camera","enabled":false},{"type":"postProcess","saturation":-1}])"));
    Access.Frame();
    if (TestNotNull(TEXT("a global look gets a volume"), Access.LookVolume()))
    {
        const APostProcessVolume* Volume = Access.LookVolume();
        TestTrue(TEXT("unbound, on"), Volume->bUnbound && Volume->bEnabled);
        TestTrue(TEXT("the global's exposure"), Volume->Settings.bOverride_AutoExposureBias
            && Volume->Settings.AutoExposureBias == -1.0f);
        TestFalse(TEXT("a switched-off camera's look is not global"),
            static_cast<bool>(Volume->Settings.bOverride_ColorSaturation));
    }
    if (TestNotNull(TEXT("the second camera"), Second->CameraComponent.Get()))
    {
        const FPostProcessSettings& Own = Second->CameraComponent->PostProcessSettings;
        TestEqual(TEXT("a camera's look applies through it"), Second->CameraComponent->PostProcessBlendWeight, 1.0f);
        TestEqual(TEXT("its own exposure on top"), Own.AutoExposureBias, 1.0f);
        TestTrue(TEXT("over the global's contrast"), Own.bOverride_ColorContrast
            && FMath::IsNearlyEqual(Own.ColorContrast.X, 1.2f));
    }
    // Hidden: the volume is switched off with its object.
    Access.Patch(TEXT("grade"), TEXT(R"([{"type":"postProcess","exposure":-1},{"type":"hidden"}])"));
    Access.Frame();
    TestFalse(TEXT("a hidden object's look applies nothing"), Access.LookVolume()->bEnabled);
    Access.Patch(TEXT("grade"), TEXT(R"([{"type":"postProcess","exposure":-1,"enabled":false}])"));
    Access.Frame();
    TestFalse(TEXT("nor does one switched off"), Access.LookVolume()->bEnabled);
    Access.Patch(TEXT("second"), TEXT(R"([{"type":"camera"}])"));
    Access.Frame();
    TestEqual(TEXT("a camera without a look of its own blends nothing"),
        Second->CameraComponent->PostProcessBlendWeight, 0.0f);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
