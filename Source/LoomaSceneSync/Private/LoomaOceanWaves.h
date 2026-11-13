#pragma once

#include "CoreMinimal.h"
#include "Math/Float16Color.h"

struct FLoomaOceanSpec;

// The four desktop bands shared by the browser's renderer and its buoyancy sampler.
// Samples are (Dx, height, Dz) in wire metres, before horizontal displacement.
class FLoomaOceanWaves
{
public:
    static constexpr int32 Size = 128;
    static constexpr int32 BandCount = 4;
    static constexpr double PeriodSeconds = 600.0;

    static double BandLength(int32 BandIndex);
    void SetSea(const FLoomaOceanSpec& Sea);
    void Evaluate(double HubSeconds);
    const TArray<FFloat16Color>& GetDisplacements(int32 BandIndex) const;

private:
    struct FMode
    {
        int32 Index;
        int32 Mirror;
        int32 Cycles;
        float Real;
        float Imaginary;
        double DirectionX;
        double DirectionZ;
    };

    struct FBand
    {
        int32 Loops = 1;
        TArray<FMode> Modes;
        TArray<FFloat16Color> Displacements;
    };

    TArray<FVector2d> Horizontal;
    TArray<FVector2d> Height;
    FBand Bands[BandCount];
    double Wind = -1.0;
    double WindDir = 0.0;
    double Swell = 0.0;
};
