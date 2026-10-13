#include "LoomaSceneAudio.h"
#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Audio.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "ISubmixBufferListener.h"
#include "Interfaces/IHttpRequest.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundGenerator.h"
#include "Sound/SoundSubmix.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_EDITOR
#include "Settings/LevelEditorMiscSettings.h"
#endif

/**
 * The `audio` component (looma-xr-asset-demo/docs/scene-format.md, "The `audio`
 * component"): parsed with the format's defaults and repairs, its WAV decoded, its
 * fade checked against UE's own evaluation of the attenuation, its sound wave played
 * from memory, and a node's source kept in step with the component as it changes.
 */

namespace
{
    FLoomaNodeComponents Parse(const TCHAR* Json)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Values);
        return LoomaParseComponents(&Values);
    }

    /** A 16-bit PCM WAV of `Samples`, interleaved, written by the engine's own writer. */
    TArray<uint8> MakeWav(const TArray<int16>& Samples, int32 Channels, int32 Rate)
    {
        TArray<uint8> Out;
        SerializeWaveFile(Out, reinterpret_cast<const uint8*>(Samples.GetData()),
            Samples.Num() * sizeof(int16), Channels, Rate);
        return Out;
    }

    /**
     * Outside Play In Editor the editor holds every game-world sound paused unless
     * real-time audio is on, and mutes everything while it is not the window in front
     * unless background audio is allowed (UEditorEngine::Tick). The viewer itself only
     * ever runs in PIE or a packaged game, and a headless editor is never in front, so
     * the playback test turns both on. Returns what they were, to put them back.
     */
    struct FEditorAudio
    {
        bool bRealTime = false;
        bool bBackground = false;
    };
    FEditorAudio SetEditorAudio(const FEditorAudio& Next)
    {
#if WITH_EDITOR
        ULevelEditorMiscSettings* Settings = GetMutableDefault<ULevelEditorMiscSettings>();
        const FEditorAudio Was{Settings->bEnableRealTimeAudio != 0, Settings->bAllowBackgroundAudio != 0};
        Settings->bEnableRealTimeAudio = Next.bRealTime;
        Settings->bAllowBackgroundAudio = Next.bBackground;
        return Was;
#else
        return Next;
#endif
    }

    FLoomaAudioSpec Spec(float Min, float Max, ELoomaAudioRolloff Rolloff)
    {
        FLoomaAudioSpec Out;
        Out.MinDistance = Min;
        Out.MaxDistance = Max;
        Out.Rolloff = Rolloff;
        return Out;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaAudioParseTest,
    "Looma.Audio.Parse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaAudioParseTest::RunTest(const FString& Parameters)
{
    // Absent fields take the format's defaults, which are not Unity's or Unreal's:
    // a sound placed in a shared scene loops and is heard where it is.
    const FLoomaNodeComponents Bare = Parse(TEXT(R"([{"type":"audio","assetId":"snd-1","url":"/api/static/snd-1.wav"}])"));
    TestTrue(TEXT("the node has an audio component"), Bare.bHasAudio);
    TestFalse(TEXT("and is not an empty object"), Bare.IsEmpty());
    TestEqual(TEXT("it names the sound"), Bare.Audio.AssetId, FString(TEXT("snd-1")));
    TestTrue(TEXT("playing by default"), Bare.Audio.bPlaying);
    TestTrue(TEXT("looping by default"), Bare.Audio.bLoop);
    TestTrue(TEXT("spatial by default"), Bare.Audio.bSpatial);
    TestEqual(TEXT("volume 1"), Bare.Audio.Volume, 1.0f);
    TestEqual(TEXT("pitch 1"), Bare.Audio.Pitch, 1.0f);
    TestEqual(TEXT("full volume within 1 m"), Bare.Audio.MinDistance, 1.0f);
    TestEqual(TEXT("fading to 20 m"), Bare.Audio.MaxDistance, 20.0f);
    TestEqual(TEXT("inverse by default"), Bare.Audio.Rolloff, ELoomaAudioRolloff::Inverse);
    TestEqual(TEXT("audio is not an unknown type"), Bare.UnknownTypes.Num(), 0);

    const FLoomaNodeComponents Set = Parse(TEXT(R"([{"type":"audio","assetId":"snd-1","playing":false,"loop":false,
        "spatial":false,"volume":0.25,"pitch":1.5,"minDistance":2,"maxDistance":8,"rolloff":"linear"}])"));
    TestFalse(TEXT("playing false"), Set.Audio.bPlaying);
    TestFalse(TEXT("loop false"), Set.Audio.bLoop);
    TestFalse(TEXT("spatial false"), Set.Audio.bSpatial);
    TestEqual(TEXT("volume"), Set.Audio.Volume, 0.25f);
    TestEqual(TEXT("pitch"), Set.Audio.Pitch, 1.5f);
    TestEqual(TEXT("minDistance"), Set.Audio.MinDistance, 2.0f);
    TestEqual(TEXT("maxDistance"), Set.Audio.MaxDistance, 8.0f);
    TestEqual(TEXT("linear"), Set.Audio.Rolloff, ELoomaAudioRolloff::Linear);

    // Repaired, never rejected: the backend's own clamps (scenegraph._clean_audio).
    const FLoomaNodeComponents Wild = Parse(TEXT(R"([{"type":"audio","assetId":"snd-1","volume":3,"pitch":0.1,
        "minDistance":0,"maxDistance":-4,"rolloff":"exponential"}])"));
    TestEqual(TEXT("volume clamps to 1"), Wild.Audio.Volume, 1.0f);
    TestEqual(TEXT("pitch clamps to 0.5"), Wild.Audio.Pitch, 0.5f);
    TestEqual(TEXT("minDistance is at least 1 cm"), Wild.Audio.MinDistance, 0.01f);
    TestEqual(TEXT("maxDistance is at least minDistance"), Wild.Audio.MaxDistance, 0.01f);
    TestEqual(TEXT("an unknown rolloff is inverse"), Wild.Audio.Rolloff, ELoomaAudioRolloff::Inverse);

    TestTrue(TEXT("the same component parses equal"),
        Bare == Parse(TEXT(R"([{"type":"audio","assetId":"snd-1"}])")));
    TestTrue(TEXT("a changed field is a change"),
        Bare != Parse(TEXT(R"([{"type":"audio","assetId":"snd-1","volume":0.5}])")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaAudioDecodeTest,
    "Looma.Audio.DecodeWav",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaAudioDecodeTest::RunTest(const FString& Parameters)
{
    const TArray<int16> Stereo = {1000, 3000, -2000, 2000, 32767, -32768, 0, 10};
    FLoomaDecodedSound Sound;
    FString Why;
    TestTrue(TEXT("a 16-bit stereo WAV decodes"), LoomaDecodeWav(MakeWav(Stereo, 2, 22050), Sound, &Why));
    TestEqual(TEXT("two channels"), Sound.NumChannels, 2);
    TestEqual(TEXT("at its own rate"), Sound.SampleRate, 22050);
    TestEqual(TEXT("four frames"), Sound.NumFrames(), 4);
    TestTrue(TEXT("sample for sample"), Sound.Samples == Stereo);

    const FLoomaDecodedSound Mono = LoomaDownmixToMono(Sound);
    TestEqual(TEXT("downmixed to one channel"), Mono.NumChannels, 1);
    TestEqual(TEXT("at the same rate"), Mono.SampleRate, 22050);
    TestTrue(TEXT("each frame the mean of its channels"), Mono.Samples == TArray<int16>({2000, 0, 0, 5}));
    TestTrue(TEXT("a mono sound is left as it is"), LoomaDownmixToMono(Mono).Samples == Mono.Samples);

    FLoomaDecodedSound Refused;
    TestFalse(TEXT("bytes that are not a WAV are refused"),
        LoomaDecodeWav(TArray<uint8>({'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v'}), Refused, &Why));

    // The backend stores only 16-bit PCM; float samples are a file from elsewhere.
    TArray<uint8> Float = MakeWav(Stereo, 2, 22050);
    Float[20] = 3; // the fmt chunk's format tag: WAVE_FORMAT_IEEE_FLOAT
    TestFalse(TEXT("a float WAV is refused"), LoomaDecodeWav(Float, Refused, &Why));
    TestTrue(TEXT("saying why"), Why.Contains(TEXT("16-bit PCM")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaAudioAttenuationTest,
    "Looma.Audio.Attenuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaAudioAttenuationTest::RunTest(const FString& Parameters)
{
    // UE's own evaluation of the attenuation against the format's formula, at
    // distances inside, along and past each fade.
    const float Distances[] = {0.0f, 0.5f, 1.0f, 1.3f, 2.0f, 3.7f, 5.0f, 8.0f, 13.0f, 19.0f, 20.0f, 35.0f, 400.0f};
    auto Check = [this, &Distances](const TCHAR* What, const FLoomaAudioSpec& Fade) {
        const FSoundAttenuationSettings Settings = LoomaMakeAttenuation(Fade);
        for (const float D : Distances)
        {
            const float Engine = Settings.Evaluate(FTransform::Identity, FVector(D * 100.0, 0.0, 0.0));
            const float Format = LoomaAudioGain(Fade, D);
            // Within 1.5% of the format, or a hair of silence: about 0.1 dB.
            TestTrue(FString::Printf(TEXT("%s at %.1f m: UE %.4f, format %.4f"), What, D, Engine, Format),
                FMath::Abs(Engine - Format) <= FMath::Max(0.015f * Format, 0.001f));
        }
        return Settings;
    };

    const FSoundAttenuationSettings Inverse = Check(TEXT("inverse 1-20"), Spec(1.0f, 20.0f, ELoomaAudioRolloff::Inverse));
    TestTrue(TEXT("inverse attenuates and spatialises"), Inverse.bAttenuate && Inverse.bSpatialize);
    TestEqual(TEXT("full volume within minDistance: the sphere"), Inverse.AttenuationShapeExtents.X, 100.0);
    TestEqual(TEXT("fading over the rest"), Inverse.FalloffDistance, 1900.0f);
    // The format holds inverse at min/max past maxDistance, so UE must not cull it there.
    TestTrue(TEXT("inverse stays audible past maxDistance"), Inverse.GetMaxDimension() > 20.0f * 100.0f * 100.0f);

    Check(TEXT("inverse 0.05-400"), Spec(0.05f, 400.0f, ELoomaAudioRolloff::Inverse));
    const FSoundAttenuationSettings Linear = Check(TEXT("linear 2-10"), Spec(2.0f, 10.0f, ELoomaAudioRolloff::Linear));
    TestEqual(TEXT("linear is UE's own"), Linear.DistanceAlgorithm, EAttenuationDistanceModel::Linear);
    TestTrue(TEXT("and silent from maxDistance on"), FMath::IsNearlyEqual(Linear.GetMaxDimension(), 1000.0f, 1.0f));
    Check(TEXT("no fade at all"), Spec(3.0f, 3.0f, ELoomaAudioRolloff::Inverse));
    Check(TEXT("no fade at all, linear"), Spec(3.0f, 3.0f, ELoomaAudioRolloff::Linear));

    FLoomaAudioSpec Flat;
    Flat.bSpatial = false;
    const FSoundAttenuationSettings Everywhere = LoomaMakeAttenuation(Flat);
    TestFalse(TEXT("a non-spatial source does not attenuate"), Everywhere.bAttenuate);
    TestFalse(TEXT("nor spatialise"), Everywhere.bSpatialize);
    TestEqual(TEXT("and is heard at volume everywhere"), LoomaAudioGain(Flat, 1000.0f), 1.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaAudioWaveTest,
    "Looma.Audio.SoundWave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaAudioWaveTest::RunTest(const FString& Parameters)
{
    FLoomaDecodedSound Sound;
    Sound.NumChannels = 1;
    Sound.SampleRate = 16000;
    Sound.Samples = {8192, 16384, -8192, -16384};
    const TArray<float> Pass = {0.25f, 0.5f, -0.25f, -0.5f};

    TStrongObjectPtr<ULoomaSoundWave> Wave(NewObject<ULoomaSoundWave>());
    Wave->SetSound(Sound);
    TestTrue(TEXT("procedural"), static_cast<bool>(Wave->bProcedural));
    TestEqual(TEXT("one channel"), Wave->NumChannels, 1);
    TestEqual(TEXT("at the sound's rate"), Wave->GetSampleRateForCurrentPlatform(), 16000.0f);
    TestEqual(TEXT("its length"), Wave->Duration, 4.0f / 16000.0f);
    TestTrue(TEXT("looping by default, so it reads as never ending"), Wave->GetDuration() >= INDEFINITELY_LOOPING_DURATION);

    float Out[10];
    auto Expect = [this, &Out](const TCHAR* What, const TArray<float>& Want) {
        for (int32 I = 0; I < Want.Num(); ++I)
        {
            TestEqual(FString::Printf(TEXT("%s, sample %d"), What, I), Out[I], Want[I]);
        }
    };

    const FSoundGeneratorInitParams Params{};
    {
        const ISoundGeneratorPtr Voice = Wave->CreateSoundGenerator(Params);
        TestEqual(TEXT("the whole buffer is filled"), Voice->OnGenerateAudio(Out, 10), 10);
        Expect(TEXT("looping goes round"), {0.25f, 0.5f, -0.25f, -0.5f, 0.25f, 0.5f, -0.25f, -0.5f, 0.25f, 0.5f});
        TestFalse(TEXT("and never finishes"), Voice->IsFinished());

        // Turned off as it plays: the pass it is on ends, then silence.
        Wave->SetLooping(false);
        Voice->OnGenerateAudio(Out, 6);
        Expect(TEXT("loop turned off finishes the pass"), {-0.25f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f});
        TestTrue(TEXT("and then the voice is finished"), Voice->IsFinished());
    }
    {
        const ISoundGeneratorPtr Once = Wave->CreateSoundGenerator(Params);
        Once->OnGenerateAudio(Out, 6);
        Expect(TEXT("a one-shot plays once"), {0.25f, 0.5f, -0.25f, -0.5f, 0.0f, 0.0f});
        TestTrue(TEXT("and finishes"), Once->IsFinished());
        TestEqual(TEXT("a one-shot reads as its length"), Wave->GetDuration(), 4.0f / 16000.0f);
    }
    return true;
}

/** Reaches into the subsystem's download queue, as the border tests reach into Tracked. */
struct FLoomaAudioTestAccess
{
    TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
    TStrongObjectPtr<ULoomaSceneSyncSubsystem> Sync{NewObject<ULoomaSceneSyncSubsystem>(GameInstance.Get())};
    UWorld* World = nullptr;

    explicit FLoomaAudioTestAccess(bool bAllowAudio = false)
    {
        const UWorld::InitializationValues Values = UWorld::InitializationValues()
            .AllowAudioPlayback(bAllowAudio).CreatePhysicsScene(false).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
        // Every download slot taken by a request that is never sent, so whatever the
        // node asks for stays in the queue to be looked at, and nothing leaves the machine.
        for (int32 Slot = 0; Slot < 4; ++Slot)
        {
            Sync->GlbInFlight.Add(FHttpModule::Get().CreateRequest());
        }
    }

    ~FLoomaAudioTestAccess()
    {
        Sync->GlbQueue.Reset();
        Sync->GlbInFlight.Reset();
        World->DestroyWorld(false);
    }

    void Apply(ALoomaSyncedActor& Actor, const TCHAR* Json)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Values);
        const FLoomaNodeComponents Components = LoomaParseComponents(&Values);
        Actor.ApplyComponents(Components, Sync->MakeRenderContext(Components));
    }

    /** The queued fetches for `Actor`, as "sound <url>" / "glb <url>". */
    TArray<FString> Queued(const ALoomaSyncedActor& Actor) const
    {
        TArray<FString> Out;
        for (const ULoomaSceneSyncSubsystem::FGlbFetch& Fetch : Sync->GlbQueue)
        {
            if (Fetch.Actor.Get() == &Actor && ULoomaSceneSyncSubsystem::IsGlbFetchWanted(Fetch))
            {
                Out.Add(FString(Fetch.bSound ? TEXT("sound ") : TEXT("glb ")) + Fetch.Url);
            }
        }
        return Out;
    }

    FString Static(const TCHAR* File) const { return Sync->GetRestBase() + TEXT("/static/") + File; }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaAudioNodeTest,
    "Looma.Audio.Node",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaAudioNodeTest::RunTest(const FString& Parameters)
{
    FLoomaAudioTestAccess Access;
    ALoomaSyncedActor* Node = Access.World->SpawnActor<ALoomaSyncedActor>();
    Node->Id = TEXT("speaker");

    // A model and its own sound: both asked for, neither replacing the other.
    Access.Apply(*Node, TEXT(R"([{"type":"model","assetId":"dog"},{"type":"audio","assetId":"snd-1","volume":0.5}])"));
    TestTrue(TEXT("the node has an audio source"), Node->AudioComponent != nullptr);
    TestNull(TEXT("with nothing to play yet"), Node->AudioComponent->Sound.Get());
    TestTrue(TEXT("the GLB and the WAV are both queued"), Access.Queued(*Node)
        == TArray<FString>({TEXT("glb ") + Access.Static(TEXT("dog.glb")), TEXT("sound ") + Access.Static(TEXT("snd-1.wav"))}));

    // The WAV arrives: stereo, played as one point because the source is spatial.
    const TArray<int16> Stereo = {1000, 3000, -2000, 2000, 500, 500};
    const TArray<uint8> Wav = MakeWav(Stereo, 2, 44100);
    Node->OnSoundDownloaded(Access.Static(TEXT("snd-1.wav")), &Wav);
    UAudioComponent* Audio = Node->AudioComponent;
    TestTrue(TEXT("the source plays the decoded sound"), Audio->Sound.Get() == Node->SoundWave.Get());
    TestEqual(TEXT("mixed down to mono, spatial"), Node->SoundWave->NumChannels, 1);
    TestTrue(TEXT("looping"), static_cast<bool>(Node->SoundWave->bLooping));
    TestEqual(TEXT("at the component's volume"), Audio->VolumeMultiplier, 0.5f);
    TestTrue(TEXT("spatialised"), static_cast<bool>(Audio->bAllowSpatialization));
    TestTrue(TEXT("with the format's fade"), Audio->bOverrideAttenuation
        && FMath::IsNearlyEqual(Audio->AttenuationOverrides.FalloffDistance, 1900.0f));

    // Edits that change a sound as it plays keep the same wave.
    ULoomaSoundWave* Before = Node->SoundWave;
    Access.Apply(*Node, TEXT(R"([{"type":"model","assetId":"dog"},{"type":"audio","assetId":"snd-1","loop":false,"pitch":2,"maxDistance":5,"rolloff":"linear"}])"));
    TestTrue(TEXT("a live edit keeps the wave"), Node->SoundWave == Before);
    TestFalse(TEXT("loop off"), static_cast<bool>(Node->SoundWave->bLooping));
    TestEqual(TEXT("pitch"), Audio->PitchMultiplier, 2.0f);
    TestEqual(TEXT("the new fade"), Audio->AttenuationOverrides.DistanceAlgorithm, EAttenuationDistanceModel::Linear);
    TestEqual(TEXT("and nothing fetched again"), Access.Queued(*Node).Num(), 2);

    // Not spatial: heard everywhere, in stereo, from a new wave.
    Access.Apply(*Node, TEXT(R"([{"type":"model","assetId":"dog"},{"type":"audio","assetId":"snd-1","spatial":false}])"));
    TestTrue(TEXT("spatial changed: a new wave"), Node->SoundWave != Before);
    TestEqual(TEXT("in stereo"), Node->SoundWave->NumChannels, 2);
    TestFalse(TEXT("not spatialised"), static_cast<bool>(Audio->bAllowSpatialization));
    TestFalse(TEXT("nor attenuated"), static_cast<bool>(Audio->bOverrideAttenuation));

    // Another sound: the old one goes at once and the new one is asked for.
    Access.Apply(*Node, TEXT(R"([{"type":"model","assetId":"dog"},{"type":"audio","assetId":"snd-2"}])"));
    TestNull(TEXT("the old sound stops"), Audio->Sound.Get());
    TestTrue(TEXT("the new sound is queued instead"), Access.Queued(*Node)
        == TArray<FString>({TEXT("glb ") + Access.Static(TEXT("dog.glb")), TEXT("sound ") + Access.Static(TEXT("snd-2.wav"))}));
    Node->OnSoundDownloaded(Access.Static(TEXT("snd-1.wav")), &Wav);
    TestNull(TEXT("a late answer for the old sound is dropped"), Audio->Sound.Get());

    // A WAV that does not decode is forgotten, so the next apply asks again.
    const TArray<uint8> Junk = {1, 2, 3};
    AddExpectedMessage(TEXT("no sound from"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
    Node->OnSoundDownloaded(Access.Static(TEXT("snd-2.wav")), &Junk);
    TestTrue(TEXT("a sound that did not decode is no longer wanted"), Node->GetWantedSoundUrl().IsEmpty());

    // The component removed: the source goes, and the model stays.
    Access.Apply(*Node, TEXT(R"([{"type":"model","assetId":"dog"}])"));
    TestNull(TEXT("no audio source"), Node->AudioComponent.Get());
    TestTrue(TEXT("only the GLB is still wanted"), Access.Queued(*Node)
        == TArray<FString>({TEXT("glb ") + Access.Static(TEXT("dog.glb"))}));
    return true;
}

namespace
{
    /** What the main submix renders: the loudest sample since asked, and the audio clock. */
    class FLoomaOutputMeter final : public ISubmixBufferListener
    {
    public:
        virtual void OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples, int32, const int32,
            double AudioClock) override
        {
            float Loudest = 0.0f;
            for (int32 Index = 0; Index < NumSamples; ++Index)
            {
                Loudest = FMath::Max(Loudest, FMath::Abs(AudioData[Index]));
            }
            FScopeLock Lock(&Guard);
            Peak = FMath::Max(Peak, Loudest);
            Clock = AudioClock;
        }

        /** The peak since the last call, and the clock now. */
        float TakePeak(double& OutClock)
        {
            FScopeLock Lock(&Guard);
            const float Out = Peak;
            Peak = 0.0f;
            OutClock = Clock;
            return Out;
        }

        double GetClock()
        {
            FScopeLock Lock(&Guard);
            return Clock;
        }

    private:
        FCriticalSection Guard;
        float Peak = 0.0f;
        double Clock = 0.0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaAudioPlaybackTest,
    "Looma.Audio.Playback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaAudioPlaybackTest::RunTest(const FString& Parameters)
{
    // Through the real mixer, measured at the main output: a one-shot is heard and then
    // finishes, and a loop is still heard long after its clip has ended. Only under the
    // engine's silent renderer (-DeterministicAudio, without -nosound), so a test run
    // never makes a sound in the room; skipped everywhere else.
    //
    // Both sources are non-spatial. That renderer mixes nothing for a 3D-panned source
    // at all, the engine's own imported sounds included (measured: /Engine/EngineSounds/
    // WhiteNoise peaks at 0.354 non-spatial and 0.000 spatial), so it cannot hear a
    // fade. The fade is Looma.Audio.Attenuation's, through UE's own evaluation of it.
    FAudioDevice* Device = GEngine ? GEngine->GetMainAudioDevice().GetAudioDevice() : nullptr;
    if (!Device || !Device->IsNonRealtime())
    {
        AddInfo(TEXT("Skipped: run with -DeterministicAudio (and without -nosound) to play through the mixer."));
        return true;
    }

    struct FState
    {
        FLoomaAudioTestAccess Access{/*bAllowAudio=*/true};
        TSharedRef<FLoomaOutputMeter, ESPMode::ThreadSafe> Meter = MakeShared<FLoomaOutputMeter, ESPMode::ThreadSafe>();
        FAudioDevice* Device = nullptr;
        TArray<uint8> Wav;
        ALoomaSyncedActor* Once = nullptr;
        ALoomaSyncedActor* Loop = nullptr;
        bool bOnceFinished = false;
        int32 Stage = 0;
        double Mark = 0.0;
        double Deadline = 0.0;
        FEditorAudio WasEditorAudio;

        ALoomaSyncedActor* Spawn(const TCHAR* Id, const TCHAR* Json)
        {
            ALoomaSyncedActor* Node = Access.World->SpawnActor<ALoomaSyncedActor>();
            Node->Id = Id;
            Access.Apply(*Node, Json);
            Node->OnSoundDownloaded(Access.Static(TEXT("tone.wav")), &Wav);
            return Node;
        }

        /** Whether `Seconds` of audio have been mixed since the mark. */
        bool Mixed(double Seconds) const { return Meter->GetClock() >= Mark + Seconds; }
    };
    const TSharedRef<FState> State = MakeShared<FState>();
    State->Device = Device;
    State->WasEditorAudio = SetEditorAudio({/*bRealTime=*/true, /*bBackground=*/true});
    Device->RegisterSubmixBufferListener(State->Meter, Device->GetMainSubmixObject());

    // A quarter of a second of 440 Hz, stereo, at half scale.
    TArray<int16> Tone;
    for (int32 Frame = 0; Frame < 12000; ++Frame)
    {
        const int16 Sample = static_cast<int16>(16000.0f * FMath::Sin(2.0f * UE_PI * 440.0f * Frame / 48000.0f));
        Tone.Add(Sample);
        Tone.Add(Sample);
    }
    State->Wav = MakeWav(Tone, 2, 48000);

    State->Once = State->Spawn(TEXT("once"), TEXT(R"([{"type":"audio","assetId":"tone","loop":false,"spatial":false}])"));
    // Weak: the state owns the world that owns the component holding this.
    const TWeakPtr<FState> Weak = State;
    State->Once->AudioComponent->OnAudioFinishedNative.AddLambda([Weak](UAudioComponent*) {
        if (const TSharedPtr<FState> Pinned = Weak.Pin())
        {
            Pinned->bOnceFinished = true;
        }
    });
    TestTrue(TEXT("the one-shot is playing"), State->Once->AudioComponent->IsPlaying());
    State->Deadline = FPlatformTime::Seconds() + 30.0;

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() {
        if (FPlatformTime::Seconds() > State->Deadline)
        {
            AddError(FString::Printf(TEXT("Timed out at stage %d"), State->Stage));
            State->Stage = 99;
        }
        double Clock = 0.0;
        switch (State->Stage)
        {
        case 0:
        {
            if (!State->bOnceFinished)
            {
                return false; // the engine ticks, and the silent renderer mixes another frame
            }
            const float Peak = State->Meter->TakePeak(Clock);
            TestTrue(FString::Printf(TEXT("the one-shot was heard (peak %.3f)"), Peak), Peak > 0.05f);
            State->Loop = State->Spawn(TEXT("loop"), TEXT(R"([{"type":"audio","assetId":"tone","spatial":false}])"));
            State->Mark = Clock;
            State->Stage = 1;
            return false;
        }
        case 1:
            // Four times the clip's length, then measure only what comes after.
            if (!State->Mixed(1.0))
            {
                return false;
            }
            State->Meter->TakePeak(State->Mark);
            State->Stage = 2;
            return false;
        case 2:
        {
            if (!State->Mixed(0.5))
            {
                return false;
            }
            const float Peak = State->Meter->TakePeak(Clock);
            TestTrue(FString::Printf(TEXT("the loop is still heard a second after it started (peak %.3f)"), Peak), Peak > 0.05f);
            TestTrue(TEXT("and still playing"), State->Loop->AudioComponent->IsPlaying());
            State->Stage = 99;
            [[fallthrough]];
        }
        default:
            for (ALoomaSyncedActor* Node : {State->Once, State->Loop})
            {
                if (Node && Node->AudioComponent)
                {
                    Node->AudioComponent->Stop();
                }
            }
            State->Device->UnregisterSubmixBufferListener(State->Meter, State->Device->GetMainSubmixObject());
            SetEditorAudio(State->WasEditorAudio);
            return true;
        }
    }));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
