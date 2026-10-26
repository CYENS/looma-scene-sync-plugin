#include "LoomaSceneAudio.h"

#include "Audio.h"
#include "Curves/RichCurve.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Sound/SoundGenerator.h"

#include <atomic>

/** Wire metres → UE centimetres, as everywhere else in the plugin. */
static constexpr float AudioWireToCm = 100.0f;

// --- Decoding -----------------------------------------------------------------

bool LoomaDecodeWav(TConstArrayView<uint8> Bytes, FLoomaDecodedSound& Out, FString* OutError)
{
    auto Fail = [OutError](const FString& Why) {
        if (OutError)
        {
            *OutError = Why;
        }
        return false;
    };

    FWaveModInfo Info;
    FString ReadError;
    if (Bytes.Num() < 44 || !Info.ReadWaveInfo(Bytes.GetData(), Bytes.Num(), &ReadError))
    {
        return Fail(ReadError.IsEmpty() ? TEXT("not a WAV file") : ReadError);
    }
    const uint16 Format = *Info.pFormatTag;
    const uint16 Bits = *Info.pBitsPerSample;
    const uint16 Channels = *Info.pChannels;
    const uint32 Rate = *Info.pSamplesPerSec;
    // WAVE_FORMAT_EXTENSIBLE (0xFFFE) carrying plain PCM is the same samples with a
    // longer header; FWaveModInfo leaves the tag as written, so take it as PCM too.
    if ((Format != FWaveModInfo::WAVE_INFO_FORMAT_PCM && Format != 0xFFFE) || Bits != 16)
    {
        return Fail(FString::Printf(TEXT("format %d at %d bits; only 16-bit PCM is played"), Format, Bits));
    }
    if (Channels < 1 || Channels > 2 || Rate < 1000 || Rate > 192000)
    {
        return Fail(FString::Printf(TEXT("%d channels at %u Hz"), Channels, Rate));
    }

    // A data chunk that claims more than the file holds is read as far as it goes.
    const int64 Available = Bytes.GetData() + Bytes.Num() - Info.SampleDataStart;
    const int64 DataBytes = FMath::Min<int64>(Info.SampleDataSize, FMath::Max<int64>(Available, 0));
    const int32 Frames = static_cast<int32>(DataBytes / (sizeof(int16) * Channels));
    if (Frames < 1)
    {
        return Fail(TEXT("no samples"));
    }

    Out.NumChannels = Channels;
    Out.SampleRate = static_cast<int32>(Rate);
    Out.Samples.SetNumUninitialized(Frames * Channels);
    // WAV is little-endian, as every platform Unreal runs this on is.
    FMemory::Memcpy(Out.Samples.GetData(), Info.SampleDataStart, Out.Samples.Num() * sizeof(int16));
    return true;
}

FLoomaDecodedSound LoomaDownmixToMono(const FLoomaDecodedSound& Sound)
{
    if (Sound.NumChannels <= 1)
    {
        return Sound;
    }
    FLoomaDecodedSound Mono;
    Mono.NumChannels = 1;
    Mono.SampleRate = Sound.SampleRate;
    const int32 Frames = Sound.NumFrames();
    Mono.Samples.SetNumUninitialized(Frames);
    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        int32 Sum = 0;
        for (int32 Channel = 0; Channel < Sound.NumChannels; ++Channel)
        {
            Sum += Sound.Samples[Frame * Sound.NumChannels + Channel];
        }
        Mono.Samples[Frame] = static_cast<int16>(Sum / Sound.NumChannels);
    }
    return Mono;
}

// --- The fade -------------------------------------------------------------------

float LoomaAudioGain(const FLoomaAudioSpec& Spec, float Distance)
{
    // The same as gainAt in the web client (frontend/src/scene/audio/engine.js).
    if (!Spec.bSpatial)
    {
        return 1.0f;
    }
    const float Min = Spec.MinDistance;
    const float Max = FMath::Max(Spec.MaxDistance, Min);
    const float D = FMath::Clamp(Distance, Min, Max);
    if (Spec.Rolloff == ELoomaAudioRolloff::Linear)
    {
        return Max <= Min ? 1.0f : 1.0f - (D - Min) / (Max - Min);
    }
    return Min / D;
}

FSoundAttenuationSettings LoomaMakeAttenuation(const FLoomaAudioSpec& Spec)
{
    FSoundAttenuationSettings Settings;
    Settings.bAttenuate = Spec.bSpatial;
    Settings.bSpatialize = Spec.bSpatial;
    Settings.AttenuationShape = EAttenuationShape::Sphere;

    const float Min = Spec.MinDistance;
    const float Max = FMath::Max(Spec.MaxDistance, Min);
    Settings.AttenuationShapeExtents = FVector(Min * AudioWireToCm, 0.0, 0.0);
    // The input to the curve is the distance past the sphere over this, 0…1, held at
    // its last value beyond. UE never divides by less than 1 cm.
    Settings.FalloffDistance = FMath::Max((Max - Min) * AudioWireToCm, 1.0f);

    // Nothing to fade over: full volume everywhere, which is what the format's
    // formulas give when the two distances are the same.
    if (Max - Min < UE_KINDA_SMALL_NUMBER)
    {
        Settings.DistanceAlgorithm = EAttenuationDistanceModel::Custom;
        FRichCurve& Curve = *Settings.CustomAttenuationCurve.GetRichCurve();
        Curve.Reset();
        Curve.AddKey(0.0f, 1.0f);
        Curve.AddKey(1.0f, 1.0f);
        return Settings;
    }

    if (Spec.Rolloff == ELoomaAudioRolloff::Linear)
    {
        Settings.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
        return Settings;
    }

    // minDistance / d, sampled at distances spaced evenly on a log scale, where the
    // curve bends evenly, and joined by straight lines. Four keys per doubling of
    // distance keeps every point within about 1% of the formula (0.1 dB).
    Settings.DistanceAlgorithm = EAttenuationDistanceModel::Custom;
    FRichCurve& Curve = *Settings.CustomAttenuationCurve.GetRichCurve();
    Curve.Reset();
    const float Ratio = Max / Min;
    const int32 Steps = FMath::Clamp(FMath::CeilToInt(FMath::Log2(Ratio) * 4.0f), 8, 96);
    for (int32 Step = 0; Step <= Steps; ++Step)
    {
        const float D = Step == Steps ? Max : Min * FMath::Pow(Ratio, static_cast<float>(Step) / Steps);
        const FKeyHandle Key = Curve.AddKey((D - Min) / (Max - Min), Min / D);
        Curve.SetKeyInterpMode(Key, RCIM_Linear);
    }
    return Settings;
}

// --- Playing from memory --------------------------------------------------------

/** One sound's samples, shared read-only by every voice that plays it. */
struct FLoomaSoundSamples
{
    /** Interleaved, already in the mixer's float range. */
    TArray<float> Samples;
    int32 NumChannels = 1;
    /** Read by the audio render thread on every buffer; written by the game thread. */
    std::atomic<bool> bLoop{true};
};

namespace
{
/** One voice: plays the samples from the start, wrapping while the flag says loop. */
class FLoomaSoundGenerator final : public ISoundGenerator
{
public:
    explicit FLoomaSoundGenerator(TSharedPtr<FLoomaSoundSamples, ESPMode::ThreadSafe> InShared)
        : Shared(MoveTemp(InShared))
    {
    }

    virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
    {
        const TArray<float>& Samples = Shared->Samples;
        int32 Written = 0;
        while (Written < NumSamples && !bFinished)
        {
            if (Position >= Samples.Num())
            {
                if (!Shared->bLoop.load(std::memory_order_relaxed) || Samples.IsEmpty())
                {
                    bFinished = true;
                    break;
                }
                Position = 0;
            }
            const int32 Count = FMath::Min(NumSamples - Written, Samples.Num() - Position);
            FMemory::Memcpy(OutAudio + Written, Samples.GetData() + Position, Count * sizeof(float));
            Position += Count;
            Written += Count;
        }
        // The mixer wants the whole buffer: silence after the last pass.
        if (Written < NumSamples)
        {
            FMemory::Memzero(OutAudio + Written, (NumSamples - Written) * sizeof(float));
        }
        return NumSamples;
    }

    virtual bool IsFinished() const override { return bFinished; }

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
    // 5.8's generator-source renderer asks the generator; 5.6 reads USoundWave's
    // NumChannels, which SetSound also fills, and has no such virtual to override.
    virtual int32 GetNumChannels() const override { return Shared->NumChannels; }
#endif

private:
    TSharedPtr<FLoomaSoundSamples, ESPMode::ThreadSafe> Shared;
    int32 Position = 0;
    bool bFinished = false;
};
} // namespace

ULoomaSoundWave::ULoomaSoundWave(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    Shared = MakeShared<FLoomaSoundSamples, ESPMode::ThreadSafe>();
    bLooping = true;
}

void ULoomaSoundWave::SetSound(const FLoomaDecodedSound& Sound)
{
    TSharedPtr<FLoomaSoundSamples, ESPMode::ThreadSafe> Next = MakeShared<FLoomaSoundSamples, ESPMode::ThreadSafe>();
    Next->NumChannels = FMath::Max(1, Sound.NumChannels);
    Next->bLoop.store(bLooping);
    Next->Samples.SetNumUninitialized(Sound.Samples.Num());
    for (int32 Index = 0; Index < Sound.Samples.Num(); ++Index)
    {
        Next->Samples[Index] = Sound.Samples[Index] / 32768.0f;
    }
    Shared = MoveTemp(Next);

    NumChannels = Shared->NumChannels;
    SetSampleRate(static_cast<uint32>(FMath::Max(1, Sound.SampleRate)));
    Duration = Sound.Duration();
    TotalSamples = static_cast<float>(Sound.NumFrames());
}

void ULoomaSoundWave::SetLooping(bool bLoop)
{
    bLooping = bLoop;
    Shared->bLoop.store(bLoop);
}

ISoundGeneratorPtr ULoomaSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
    return MakeShared<FLoomaSoundGenerator, ESPMode::ThreadSafe>(Shared);
}
