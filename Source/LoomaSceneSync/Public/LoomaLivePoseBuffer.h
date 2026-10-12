#pragma once

#include "CoreMinimal.h"

/**
 * A live pose another client published for a rigged character (HAM-316): the
 * wire's `pose`, docs/rig-format.md "Live pose" in looma-xr-asset-demo. Still in
 * the wire's terms, which are not Unreal's: whoever poses a mesh from it does
 * the last step, into that mesh's own bones.
 */
struct FLoomaLivePose
{
    /** Left, up, forward, in units of the character's leg length. */
    FVector Hips = FVector::ZeroVector;
    /** The 22 body bones' local rotations in the rig format's order and the rig FILE's axes, from the T-pose. */
    TArray<FQuat> Rotations;
    /** FPlatformTime::Seconds() when it arrived. */
    double ReceivedAt = 0.0;
    /** The usual time between this node's frames, in seconds; 0 until two have come. */
    double Gap = 0.0;
};

/**
 * The last few poses received for one node, played back a little in the past
 * (HAM-325).
 *
 * Poses are sent at a steady rate and arrive unevenly: the network holds some
 * back, and a client only reads its socket once a rendered frame. A character
 * that chases the newest pose speeds up and slows down with the arrivals. This
 * shows instead where the character was a short time ago, between two received
 * poses, so the unevenness is spent in the waiting and not in the movement.
 *
 * The wire carries no times, so each pose is given one: a clock that ticks once
 * a pose at the usual gap, and is pulled gently towards when poses really
 * arrive. How far in the past to play is one gap, so there is always a pose on
 * each side, plus three times how far arrivals stray from that clock: next to
 * nothing on a good network, more on a bad one.
 *
 * No socket and no clock of its own: the caller says what time it is, which is
 * what lets a test drive it.
 */
struct LOOMASCENESYNC_API FLoomaLivePoseBuffer
{
    /** How long after the newest pose the node is let go, in seconds. */
    static constexpr double StaleSeconds = 1.0;
    /** The furthest in the past a character is ever shown, in seconds. */
    static constexpr double LongestDelay = 0.3;

    /** A pose that arrived at `Now`. Its ReceivedAt and Gap are not read. */
    void Push(const FLoomaLivePose& Pose, double Now);

    /**
     * The pose to show at `Now`, or false if there is none or the newest is
     * older than StaleSeconds. `Out.ReceivedAt` is when the newest arrived and
     * `Out.Gap` the usual time between poses.
     */
    bool Sample(double Now, FLoomaLivePose& Out) const;

    /** The newest pose as it arrived, or false as Sample. */
    bool Newest(double Now, FLoomaLivePose& Out) const;

    /** How far in the past Sample is showing, in seconds. */
    double GetDelay() const { return Delay; }
    /** How far arrivals stray from a steady clock, on average, in seconds. */
    double GetJitter() const { return Jitter; }
    double GetGap() const { return Gap; }

private:
    struct FFrame
    {
        FVector Hips = FVector::ZeroVector;
        TArray<FQuat> Rotations;
        /** On the evened-out clock, not when it arrived. */
        double Time = 0.0;
    };
    /** Oldest first. */
    TArray<FFrame> Frames;
    double LastArrival = 0.0;
    /** Poses since the first of this performance. */
    int32 Received = 0;
    double Gap = 0.0;
    double Jitter = 0.0;
    double Delay = 0.0;
};
