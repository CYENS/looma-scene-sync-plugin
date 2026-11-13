#include "LoomaOceanWaves.h"

#include "LoomaSceneComponents.h"

#include <cmath>

// Spectrum and seeded noise follow asset-demo's spectrum.js, adapted from
// web-ocean (MIT, Copyright 2026 Maksymilian Dendura; docs/LICENSE-web-ocean).
namespace
{
constexpr double Pi = UE_DOUBLE_PI;
constexpr double Tau = 2.0 * Pi;
constexpr double Gravity = 9.81;
constexpr double SurfaceTension = 7.4e-5;
constexpr double Choppiness = 1.25;
constexpr int32 N = FLoomaOceanWaves::Size;
constexpr int32 Period = static_cast<int32>(FLoomaOceanWaves::PeriodSeconds);
// The fifth length sets the upper frequency of the last band we render.
constexpr double Lengths[] = {1103.0, 211.3, 39.7, 7.43, 1.393};

double Omega(double K)
{
    return FMath::Sqrt(Gravity * K + SurfaceTension * K * K * K);
}

double WrapAngle(double Angle)
{
    return Angle - Tau * FMath::FloorToDouble((Angle + Pi) / Tau);
}

double Jonswap(double W, double Peak, double Alpha, double Gamma)
{
    const double Sigma = W <= Peak ? 0.07 : 0.09;
    const double D = (W - Peak) / (Sigma * Peak);
    const double Q2 = FMath::Square(Peak / W);
    return Alpha * Gravity * Gravity / (W * W * W * W * W)
        * FMath::Exp(-1.25 * Q2 * Q2) * FMath::Pow(Gamma, FMath::Exp(-0.5 * D * D));
}

double DonelanBanner(double Angle, double Ratio)
{
    double Beta;
    if (Ratio < 0.95)
    {
        Beta = 2.61 * FMath::Pow(Ratio, 1.3);
    }
    else if (Ratio < 1.6)
    {
        Beta = 2.28 * FMath::Pow(Ratio, -1.3);
    }
    else
    {
        Beta = FMath::Pow(10.0, -0.4 + 0.8393 * FMath::Exp(-0.567 * FMath::Loge(Ratio * Ratio)));
    }
    const double Sh = std::cosh(FMath::Clamp(Beta * Angle, -20.0, 20.0));
    return Beta / (2.0 * std::tanh(Beta * Pi) * Sh * Sh);
}

uint32 PcgHash(uint32 Value)
{
    const uint32 S = Value * 747796405u + 2891336453u;
    const uint32 W = ((S >> ((S >> 28u) + 4u)) ^ S) * 277803737u;
    return (W >> 22u) ^ W;
}

double Random01(uint32 Seed)
{
    return ((PcgHash(Seed) >> 8u) + 0.5) / 16777216.0;
}

FVector2d Gaussian(int32 X, int32 Z, int32 Band)
{
    const uint32 WaveKey = PcgHash((static_cast<uint32>(X) & 0xffffu) << 16u | (static_cast<uint32>(Z) & 0xffffu));
    const uint32 Seed = 7919u + Band * 1000003u + WaveKey;
    const double Radius = FMath::Sqrt(-2.0 * FMath::Loge(Random01(Seed * 2u + 17u)));
    const double Angle = Tau * Random01(Seed * 2u + 18u + PcgHash(Seed));
    return FVector2d(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle));
}

struct FFft
{
    FVector2d Twiddles[N / 2];
    int32 Reverse[N];

    FFft()
    {
        Reverse[0] = 0;
        for (int32 I = 1; I < N; ++I)
        {
            Reverse[I] = (Reverse[I >> 1] >> 1) | ((I & 1) * (N / 2));
        }
        for (int32 I = 0; I < N / 2; ++I)
        {
            const double Angle = Tau * I / N;
            Twiddles[I] = FVector2d(FMath::Cos(Angle), FMath::Sin(Angle));
        }
    }

    // Positive exponent, unnormalised, as in asset-demo's fft.js.
    void Transform(FVector2d* Row) const
    {
        for (int32 I = 1; I < N; ++I)
        {
            if (I < Reverse[I])
            {
                Swap(Row[I], Row[Reverse[I]]);
            }
        }
        for (int32 Width = 2; Width <= N; Width *= 2)
        {
            const int32 Half = Width / 2;
            const int32 Step = N / Width;
            for (int32 Start = 0; Start < N; Start += Width)
            {
                for (int32 K = 0; K < Half; ++K)
                {
                    FVector2d& A = Row[Start + K];
                    FVector2d& B = Row[Start + K + Half];
                    const FVector2d W = Twiddles[K * Step];
                    const FVector2d V(B.X * W.X - B.Y * W.Y, B.X * W.Y + B.Y * W.X);
                    B = A - V;
                    A += V;
                }
            }
        }
    }

    void Transform2D(TArray<FVector2d>& Field) const
    {
        FVector2d Column[N];
        for (int32 I = 0; I < N; ++I)
        {
            Transform(Field.GetData() + I * N);
        }
        for (int32 X = 0; X < N; ++X)
        {
            for (int32 Z = 0; Z < N; ++Z)
            {
                Column[Z] = Field[Z * N + X];
            }
            Transform(Column);
            for (int32 Z = 0; Z < N; ++Z)
            {
                Field[Z * N + X] = Column[Z];
            }
        }
    }
};
} // namespace

double FLoomaOceanWaves::BandLength(int32 BandIndex)
{
    return Lengths[BandIndex];
}

const TArray<FFloat16Color>& FLoomaOceanWaves::GetDisplacements(int32 BandIndex) const
{
    return Bands[BandIndex].Displacements;
}

void FLoomaOceanWaves::SetSea(const FLoomaOceanSpec& Sea)
{
    if (Wind == Sea.Wind && WindDir == Sea.WindDir && Swell == Sea.Swell)
    {
        return;
    }
    Wind = Sea.Wind;
    WindDir = Sea.WindDir;
    Swell = Sea.Swell;

    constexpr double Fetch = 30000.0;
    const double WindAngle = FMath::DegreesToRadians(WindDir);
    const double WindAlpha = 0.076 * FMath::Pow(Wind * Wind / (Fetch * Gravity), 0.22);
    const double WindPeak = Wind > 0.1 ? 22.0 * FMath::Pow(Gravity * Gravity / (Wind * Fetch), 1.0 / 3.0) : 0.0;
    const double SwellPeak = FMath::Sqrt(Gravity * Tau / 90.0);
    const double SwellAlpha = FMath::Square(Swell * 0.25) * 5.0 * FMath::Square(FMath::Square(SwellPeak) / Gravity)
        * (1.0 - 0.287 * FMath::Loge(6.0));
    const double SwellAngle = FMath::DegreesToRadians(94.0);
    const double SwellSpread = FMath::Sqrt(48.25) / (2.0 * FMath::Sqrt(Pi));

    for (int32 B = 0; B < BandCount; ++B)
    {
        FBand& Band = Bands[B];
        Band.Modes.Reset();
        const double DK = Tau / Lengths[B];
        const double KMin = (B == 0 ? Pi : Tau * 6.0) / Lengths[B];
        const double KMax = Tau * 6.0 / Lengths[B + 1];
        int32 Loops = FMath::Max(1, FMath::FloorToInt(0.01 * Omega(KMin) * Period / Pi));
        while (Period % Loops != 0)
        {
            --Loops;
        }
        Band.Loops = Loops;
        const double LoopSeconds = static_cast<double>(Period) / Loops;

        for (int32 Z = -N / 2; Z < N / 2; ++Z)
        {
            for (int32 X = -N / 2; X < N / 2; ++X)
            {
                const double KX = X * DK;
                const double KZ = Z * DK;
                const double K = std::hypot(KX, KZ);
                if (K < KMin || K >= KMax)
                {
                    continue;
                }
                const double W = Omega(K);
                const double Angle = std::atan2(KZ, KX);
                double Density = 0.0;
                if (WindPeak > 0.0)
                {
                    Density = Jonswap(W, WindPeak, WindAlpha, 3.3) * DonelanBanner(WrapAngle(Angle - WindAngle), W / WindPeak);
                }
                if (Swell > 0.001)
                {
                    const double Spread = SwellSpread * FMath::Pow(FMath::Abs(FMath::Cos(WrapAngle(Angle - SwellAngle) * 0.5)), 96.0);
                    Density += Jonswap(W, SwellPeak, SwellAlpha, 6.0) * Spread;
                }
                Density *= FMath::Exp(-FMath::Square(K * 0.0012)) * (Gravity + 3.0 * SurfaceTension * K * K) / (2.0 * W * K);
                const double Amplitude = FMath::Sqrt(FMath::Max(Density, 0.0)) * DK * 0.5;
                if (Amplitude == 0.0)
                {
                    continue;
                }
                const FVector2d H0 = Gaussian(X, Z, B) * Amplitude;
                Band.Modes.Add({
                    ((Z + N) % N) * N + ((X + N) % N),
                    ((N - Z) % N) * N + ((N - X) % N),
                    FMath::RoundToInt32(W * LoopSeconds / Tau),
                    static_cast<float>(H0.X), static_cast<float>(H0.Y),
                    Choppiness * KX / K, Choppiness * KZ / K,
                });
            }
        }
    }
}

void FLoomaOceanWaves::Evaluate(double HubSeconds)
{
    static const FFft Fft;
    const double Phase = FMath::Fmod(FMath::Fmod(HubSeconds, PeriodSeconds) + PeriodSeconds, PeriodSeconds) / PeriodSeconds;
    for (FBand& Band : Bands)
    {
        Band.Displacements.SetNumUninitialized(N * N);
        if (Band.Modes.IsEmpty())
        {
            FMemory::Memzero(Band.Displacements.GetData(), N * N * sizeof(FFloat16Color));
            continue;
        }
        Horizontal.Init(FVector2d::ZeroVector, N * N);
        Height.Init(FVector2d::ZeroVector, N * N);

        const double CascadePhase = static_cast<float>(FMath::Frac(Phase * Band.Loops));
        for (const FMode& Mode : Band.Modes)
        {
            const double Angle = Tau * FMath::Frac(Mode.Cycles * CascadePhase);
            const double C = FMath::Cos(Angle);
            const double S = FMath::Sin(Angle);
            const double Real = Mode.Real * C + Mode.Imaginary * S;
            const double Imaginary = Mode.Imaginary * C - Mode.Real * S;

            // h(k) and its conjugate at -k. Pack Dx + i·Dz in the second FFT.
            Height[Mode.Index] += FVector2d(Real, Imaginary);
            Height[Mode.Mirror] += FVector2d(Real, -Imaginary);
            Horizontal[Mode.Index] += FVector2d(
                -Imaginary * Mode.DirectionX - Real * Mode.DirectionZ,
                Real * Mode.DirectionX - Imaginary * Mode.DirectionZ);
            Horizontal[Mode.Mirror] += FVector2d(
                -Imaginary * Mode.DirectionX + Real * Mode.DirectionZ,
                -Real * Mode.DirectionX - Imaginary * Mode.DirectionZ);
        }
        Fft.Transform2D(Horizontal);
        Fft.Transform2D(Height);
        for (int32 I = 0; I < N * N; ++I)
        {
            Band.Displacements[I] = FFloat16Color(FLinearColor(Horizontal[I].X, Height[I].X, Horizontal[I].Y, 0.0f));
        }
    }
}
