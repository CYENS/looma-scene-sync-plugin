#include "LoomaSceneSyncSettings.h"

#include "LoomaSceneSyncLog.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
/** Where these knobs lived when they were UPROPERTY(Config) on the subsystem. */
const TCHAR* LegacyConfigSection = TEXT("/Script/LoomaSceneSync.LoomaSceneSyncSubsystem");
} // namespace

ULoomaSceneSyncSettings::ULoomaSceneSyncSettings()
{
    // Project Settings > Plugins > Looma Scene Sync.
    CategoryName = TEXT("Plugins");
    SectionName = TEXT("LoomaSceneSync");
}

const ULoomaSceneSyncSettings& ULoomaSceneSyncSettings::Get()
{
    return *GetDefault<ULoomaSceneSyncSettings>();
}

FSimpleMulticastDelegate& ULoomaSceneSyncSettings::OnSettingsChanged()
{
    static FSimpleMulticastDelegate Delegate;
    return Delegate;
}

void ULoomaSceneSyncSettings::PostInitProperties()
{
    Super::PostInitProperties();
    if (HasAnyFlags(RF_ClassDefaultObject) && GConfig)
    {
        ImportLegacyConfig();
    }
}

void ULoomaSceneSyncSettings::ImportLegacyConfig()
{
    const FString Section = GetClass()->GetPathName();
    // Only fill in keys this class's own section is silent about — an explicit value
    // here (even one equal to the default) must not be overridden by a stale one.
    const auto Unset = [&Section](const TCHAR* Key) {
        FString Ignored;
        return !GConfig->GetString(*Section, Key, Ignored, GGameIni);
    };

    bool bFoundAny = false;

    FString Host;
    if (Unset(TEXT("BackendUrl")) && GConfig->GetString(LegacyConfigSection, TEXT("BackendHost"), Host, GGameIni))
    {
        // The old key held a bare host:port, which BackendUrl accepts as-is.
        BackendUrl = Host;
        bFoundAny = true;
    }
    float Scale = 0.0f;
    if (Unset(TEXT("LightIntensityScale")) &&
        GConfig->GetFloat(LegacyConfigSection, TEXT("LightIntensityScale"), Scale, GGameIni))
    {
        LightIntensityScale = Scale;
        bFoundAny = true;
    }
    bool bAlign = false;
    if (Unset(TEXT("bBaseAlignModels")) &&
        GConfig->GetBool(LegacyConfigSection, TEXT("bBaseAlignModels"), bAlign, GGameIni))
    {
        bBaseAlignModels = bAlign;
        bFoundAny = true;
    }
    FString Prefix;
    if (Unset(TEXT("WebAssetPrefix")) && GConfig->GetString(LegacyConfigSection, TEXT("WebAssetPrefix"), Prefix, GGameIni))
    {
        WebAssetPrefix = Prefix;
        bFoundAny = true;
    }

    if (bFoundAny)
    {
        UE_LOG(LogLoomaSync, Log,
            TEXT("Read settings from the legacy [%s] .ini section. Project Settings > Plugins > Looma Scene Sync ")
            TEXT("writes [%s]; move them there and drop the old section."),
            LegacyConfigSection, *Section);
    }
}

#if WITH_EDITOR
void ULoomaSceneSyncSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    // The Project Settings panel saves the section itself; do it here too so an edit
    // made through any other details view (or via script) also lands in DefaultGame.ini.
    TryUpdateDefaultConfigFile();
    OnSettingsChanged().Broadcast();
}
#endif
