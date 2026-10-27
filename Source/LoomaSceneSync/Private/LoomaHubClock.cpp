#include "LoomaHubClock.h"

#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"

double FLoomaHubClock::LocalMs()
{
    // Anchored once to the wall clock, then carried by the monotonic counter.
    static const double EpochMsAtStart =
        (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds() - FPlatformTime::Seconds() * 1000.0;
    return EpochMsAtStart + FPlatformTime::Seconds() * 1000.0;
}

bool FLoomaHubClock::AddSample(double T0Ms, double ServerMs, double T3Ms)
{
    if (!FMath::IsFinite(T0Ms) || !FMath::IsFinite(ServerMs) || !FMath::IsFinite(T3Ms))
    {
        return false;
    }
    const double RoundTrip = T3Ms - T0Ms;
    if (RoundTrip < 0.0 || RoundTrip > MaxRoundTripMs)
    {
        return false;
    }
    Samples.Add({ ServerMs - (T0Ms + T3Ms) / 2.0, RoundTrip });
    if (Samples.Num() > Keep)
    {
        Samples.RemoveAt(0, Samples.Num() - Keep);
    }
    // The shortest round trip is the answer that queued least on the way.
    const FSample* Best = &Samples[0];
    for (const FSample& Sample : Samples)
    {
        if (Sample.RoundTripMs < Best->RoundTripMs)
        {
            Best = &Sample;
        }
    }
    OffsetMs = Best->OffsetMs;
    RoundTripMs = Best->RoundTripMs;
    bSynced = true;
    return true;
}

void FLoomaHubClock::Reset()
{
    Samples.Reset();
    bSynced = false;
    OffsetMs = 0.0;
    RoundTripMs = 0.0;
}
