#if WITH_DEV_AUTOMATION_TESTS

#include "LoomaOceanWaves.h"
#include "LoomaSceneComponents.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaOceanWavesTest, "Looma.Ocean.BrowserParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaOceanWavesTest::RunTest(const FString& Parameters)
{
    struct FProbe
    {
        int32 X;
        int32 Z;
        FVector3d Expected;
    };
    struct FCase
    {
        double HubMilliseconds;
        float Wind;
        double WindDir;
        float Swell;
        FProbe Bands[FLoomaOceanWaves::BandCount];
    };

    // asset-demo's seaFields(buildH0(band, sea), band, fround(cascadePhase(seaPhase(t), band)))
    // at desktop texel (2*X, 2*Z). Unreal's 128 grid shares those points with the 256 grid.
    const FCase Cases[] = {
        {1794567256123.5, 5.0f, 118.0, 0.4f, {
            {0, 0, FVector3d(0.032633413, -0.095026342, 0.111211488)},
            {13, 29, FVector3d(-0.040557737, 0.037105959, -0.197993937)},
            {91, 54, FVector3d(0.009734101, 0.015288826, 0.106978582)},
            {78, 123, FVector3d(0.004756342, 0.011330246, -0.003086164)},
        }},
        {1794567599999.5, 10.0f, 90.0, 1.25f, {
            {4, 119, FVector3d(-0.087839390, -0.174182257, 0.441373647)},
            {51, 39, FVector3d(0.004138441, -0.210166244, 0.112924504)},
            {127, 64, FVector3d(-0.005274582, 0.065894831, -0.021816859)},
            {16, 57, FVector3d(0.008599572, -0.014253849, -0.009255529)},
        }},
        {-123456.25, 0.0f, 118.0, 1.5f, {
            {18, 44, FVector3d(-0.077884491, 0.044780929, 0.028778979)},
            {94, 35, FVector3d(0.029252756, -0.045444051, 0.077396423)},
            {11, 123, FVector3d(0.001561132, 0.039803251, 0.017445512)},
            {90, 37, FVector3d(0.001872786, 0.002804653, 0.000587962)},
        }},
    };

    FLoomaOceanWaves Waves;
    FLoomaOceanSpec Sea;
    for (const FCase& Case : Cases)
    {
        Sea.Wind = Case.Wind;
        Sea.WindDir = Case.WindDir;
        Sea.Swell = Case.Swell;
        Waves.SetSea(Sea);
        Waves.Evaluate(Case.HubMilliseconds / 1000.0);
        for (int32 Band = 0; Band < FLoomaOceanWaves::BandCount; ++Band)
        {
            const FProbe& Probe = Case.Bands[Band];
            const FFloat16Color& Sample = Waves.GetDisplacements(Band)[Probe.Z * FLoomaOceanWaves::Size + Probe.X];
            const FVector3d Actual(Sample.R.GetFloat(), Sample.G.GetFloat(), Sample.B.GetFloat());
            TestEqual(FString::Printf(TEXT("browser displacement at %.2f ms, band %d"), Case.HubMilliseconds, Band),
                Actual, Probe.Expected, 0.00025);
        }
    }

    const FFloat16Color First = Waves.GetDisplacements(0)[0];
    Waves.Evaluate(Cases[2].HubMilliseconds / 1000.0 + FLoomaOceanWaves::PeriodSeconds);
    const FFloat16Color Second = Waves.GetDisplacements(0)[0];
    TestTrue(TEXT("the same sample repeats after the browser's 600-second period"), First == Second);

    Sea.Swell = 0.0f;
    Waves.SetSea(Sea);
    Waves.Evaluate(12.0);
    const FFloat16Color Calm = Waves.GetDisplacements(0)[0];
    TestTrue(TEXT("a calm sea clears previously animated displacement"), Calm == FFloat16Color(FLinearColor(0, 0, 0, 0)));
    return true;
}

#endif
