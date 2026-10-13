#pragma once

#include "CoreMinimal.h"
#include "LoomaSceneComponents.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "LoomaSceneAudio.generated.h"

/**
 * What the `audio` component needs from Unreal's audio engine (scene format v3 —
 * looma-xr-asset-demo/docs/scene-format.md, "The `audio` component"): a datalake
 * sound decoded and made playable, and the format's fade as an attenuation.
 *
 * Kept apart from ALoomaSyncedActor so each piece can be tested on its own, the fade
 * against UE's own evaluation of it.
 */

/**
 * A datalake sound, decoded. Every stored sound is the backend's canonical WAV
 * (looma-xr-asset-demo/backend/app/audio.py): 16-bit PCM, one or two channels, at its
 * source's own sample rate.
 */
struct LOOMASCENESYNC_API FLoomaDecodedSound
{
    /** Interleaved, frame by frame, as the WAV stores them. */
    TArray<int16> Samples;
    int32 NumChannels = 0;
    int32 SampleRate = 0;

    int32 NumFrames() const { return NumChannels > 0 ? Samples.Num() / NumChannels : 0; }
    float Duration() const { return SampleRate > 0 ? static_cast<float>(NumFrames()) / SampleRate : 0.0f; }
    bool IsValid() const { return NumChannels > 0 && SampleRate > 0 && NumFrames() > 0; }
};

/**
 * Read a 16-bit PCM WAV, through the engine's own reader (FWaveModInfo). False, with
 * the reason in `OutError`, for anything else: the backend stores nothing else, so
 * another format here is a file that did not come from it.
 */
LOOMASCENESYNC_API bool LoomaDecodeWav(TConstArrayView<uint8> Bytes, FLoomaDecodedSound& Out, FString* OutError = nullptr);

/**
 * The same sound as one channel, each frame the mean of its channels. A spatial
 * source is one point, so a stereo clip is mixed down before it is placed in the
 * room, as the web client does with a panner whose input is one channel.
 */
LOOMASCENESYNC_API FLoomaDecodedSound LoomaDownmixToMono(const FLoomaDecodedSound& Sound);

/** The format's fade at `Distance` wire metres, 0…1: what every client should hear. */
LOOMASCENESYNC_API float LoomaAudioGain(const FLoomaAudioSpec& Spec, float Distance);

/**
 * The attenuation that makes Unreal fade an `audio` component as the format does: a
 * sphere of `minDistance`, falling off over `maxDistance - minDistance`.
 *
 * `linear` is UE's own Linear algorithm. `inverse` is not UE's Inverse, which is a
 * different formula (0.02 / (d / falloff)); it is drawn as a custom curve of
 * minDistance / d instead. A curve that ends above zero also keeps the sound audible
 * past maxDistance at minDistance / maxDistance, which is what the format says
 * `inverse` does there.
 */
LOOMASCENESYNC_API FSoundAttenuationSettings LoomaMakeAttenuation(const FLoomaAudioSpec& Spec);

struct FLoomaSoundSamples;

/**
 * A decoded sound, playable by a UAudioComponent: the sound is generated from memory
 * rather than imported, because it arrives at runtime from the backend's /static.
 *
 * Looping is read by the generator as it plays, so `loop` changes on a sound that is
 * already playing, as it does in the browser: turned off, the sound finishes the pass
 * it is on and stops; turned on, it carries on round.
 */
UCLASS(Transient)
class LOOMASCENESYNC_API ULoomaSoundWave : public USoundWaveProcedural
{
    GENERATED_BODY()

public:
    ULoomaSoundWave(const FObjectInitializer& ObjectInitializer);

    /** Take `Sound` as this wave's samples. Once, before the wave is first played. */
    void SetSound(const FLoomaDecodedSound& Sound);

    /** Loop or not, on every voice of this wave, including ones already playing. */
    void SetLooping(bool bLoop);

    virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
    /** The samples and the loop flag, shared with every voice playing them. */
    TSharedPtr<FLoomaSoundSamples, ESPMode::ThreadSafe> Shared;
};
