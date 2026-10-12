#include "LoomaLivePoseBuffer.h"

namespace
{
    // Enough for LongestDelay at the hub's fastest, with room to spare.
    const int32 MostFrames = 32;
    // The usual gap is the average over this many poses: uneven arrivals cancel
    // out of an average, where a quicker measure would wobble with them.
    const int32 GapPoses = 120;
    // How fast the stray and the delay follow what they measure, a pose at a
    // time. Slow, so one late pose changes nothing anyone can see.
    const double JitterFollow = 0.1;
    const double DelayFollow = 0.05;
    // How fast the evened-out clock is pulled towards real arrivals.
    const double ClockFollow = 0.05;
    // These were chosen in a Python model of this file: a steady movement sent 30
    // times a second, each pose up to 40 ms late, watched at 60 frames a second,
    // plays back at 0.91 to 1.05 of its speed, 70 ms behind. Taking the newest
    // pose instead gives 0 to 4 times its speed.
    // The delay covers this many times the average stray, so a pose that is
    // later than most is still there when its turn comes.
    const double JitterCover = 3.0;
}

void FLoomaLivePoseBuffer::Push(const FLoomaLivePose& Pose, double Now)
{
    FFrame Frame;
    Frame.Hips = Pose.Hips;
    Frame.Rotations = Pose.Rotations;

    const double Since = Now - LastArrival;
    if (Frames.Num() == 0 || Since < 0.0 || Since >= StaleSeconds)
    {
        // A first pose, or the first after a silence: a new performance.
        Frames.Reset();
        Gap = Jitter = Delay = 0.0;
        Received = 0;
        Frame.Time = Now;
    }
    else
    {
        const bool bSecond = ++Received == 1;
        Gap = bSecond ? Since : Gap + (Since - Gap) / FMath::Min(Received, GapPoses);
        // Where a steady sender's clock puts this pose, pulled a little towards
        // when it really came. Never before the pose it follows.
        const double Expected = Frames.Last().Time + Gap;
        const double Stray = Now - Expected;
        if (FMath::Abs(Stray) > 3.0 * Gap + 0.1)
        {
            // The sender stopped and started, or changed its rate: start the clock again.
            Frame.Time = Now;
        }
        else
        {
            Frame.Time = FMath::Max(Frames.Last().Time, Expected + ClockFollow * Stray);
            Jitter += JitterFollow * (FMath::Abs(Stray) - Jitter);
        }
        const double Wanted = FMath::Min(Gap + JitterCover * Jitter, LongestDelay);
        Delay = bSecond ? Wanted : Delay + DelayFollow * (Wanted - Delay);
    }
    LastArrival = Now;
    if (Frames.Num() >= MostFrames)
    {
        Frames.RemoveAt(0, EAllowShrinking::No);
    }
    Frames.Add(MoveTemp(Frame));
}

bool FLoomaLivePoseBuffer::Newest(double Now, FLoomaLivePose& Out) const
{
    if (Frames.Num() == 0 || Now - LastArrival > StaleSeconds)
    {
        return false;
    }
    Out.Hips = Frames.Last().Hips;
    Out.Rotations = Frames.Last().Rotations;
    Out.ReceivedAt = LastArrival;
    Out.Gap = Gap;
    return true;
}

bool FLoomaLivePoseBuffer::Sample(double Now, FLoomaLivePose& Out) const
{
    if (!Newest(Now, Out))
    {
        return false;
    }
    const double At = Now - Delay;
    // The last pose at or before the time being shown.
    int32 Before = INDEX_NONE;
    for (int32 Index = Frames.Num() - 1; Index >= 0; --Index)
    {
        if (Frames[Index].Time <= At)
        {
            Before = Index;
            break;
        }
    }
    if (Before == Frames.Num() - 1)
    {
        return true; // nothing newer has come: hold the newest
    }
    if (Before == INDEX_NONE)
    {
        // Everything kept is newer: the oldest is the nearest there is.
        Out.Hips = Frames[0].Hips;
        Out.Rotations = Frames[0].Rotations;
        return true;
    }
    const FFrame& A = Frames[Before];
    const FFrame& B = Frames[Before + 1];
    const double Span = B.Time - A.Time;
    const double T = Span > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((At - A.Time) / Span, 0.0, 1.0) : 1.0;
    Out.Hips = FMath::Lerp(A.Hips, B.Hips, T);
    Out.Rotations = B.Rotations;
    if (A.Rotations.Num() == B.Rotations.Num())
    {
        for (int32 Index = 0; Index < B.Rotations.Num(); ++Index)
        {
            Out.Rotations[Index] = FQuat::Slerp(A.Rotations[Index], B.Rotations[Index], T);
        }
    }
    return true;
}
