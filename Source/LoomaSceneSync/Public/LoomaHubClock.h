#pragma once

#include "CoreMinimal.h"

/**
 * The hub's clock on this client (HAM-330; looma-xr-asset-demo/docs/scene-format.md,
 * "`clock`"). A path's carriage is wherever the hub's stamps say it is at the hub's
 * now, so "now" has to be the hub's, not this machine's, or two screens show two
 * moments of one move.
 *
 * NTP-style, as the web client does it (frontend/src/sync/clock.js): send
 * `{type: "clock", t0}` with this machine's time, the hub answers `{t0, server}` at
 * once, and with the time the answer arrived (t3)
 *
 *     offset = server − (t0 + t3) / 2
 *
 * which is exact when the two legs take equally long and wrong by at most half the
 * round trip when they do not. Of the last few samples, the one with the shortest
 * round trip is kept. Pure state: the subsystem sends the pings and feeds the answers.
 */
struct LOOMASCENESYNC_API FLoomaHubClock
{
    /** How many recent samples are weighed. */
    static constexpr int32 Keep = 8;
    /** A sample whose round trip took longer than this says nothing useful. */
    static constexpr double MaxRoundTripMs = 5000.0;

    /**
     * This machine's clock in milliseconds since the Unix epoch, but steady: it starts
     * at the wall clock and then advances with FPlatformTime, so a clock set mid-session
     * does not move it. The web's `performance.timeOrigin + performance.now()`.
     */
    static double LocalMs();

    /** One answer: our `T0Ms` echoed, the hub's `ServerMs`, and `T3Ms` when it arrived. False if refused. */
    bool AddSample(double T0Ms, double ServerMs, double T3Ms);

    /** Whether any answer has been kept yet. */
    bool IsSynced() const { return bSynced; }

    /** The hub's clock minus ours, ms. 0 until synced. */
    double GetOffsetMs() const { return OffsetMs; }

    /** The round trip of the kept sample, ms. */
    double GetRoundTripMs() const { return RoundTripMs; }

    /**
     * The hub's wall clock, ms, at `LocalNowMs` on ours. Until a sample exists, ours: a
     * carriage then starts where this machine thinks it should, and slides into step
     * when the first answer lands.
     */
    double ServerNowMs(double LocalNowMs) const { return LocalNowMs + OffsetMs; }

    /** Forget every sample, for a new socket. */
    void Reset();

private:
    struct FSample
    {
        double OffsetMs = 0.0;
        double RoundTripMs = 0.0;
    };
    TArray<FSample> Samples;
    bool bSynced = false;
    double OffsetMs = 0.0;
    double RoundTripMs = 0.0;
};
