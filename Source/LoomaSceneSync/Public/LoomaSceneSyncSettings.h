#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LoomaSceneSyncSettings.generated.h"

/**
 * Project Settings > Plugins > Looma Scene Sync — every knob the plugin reads,
 * in one place, saved to `Config/DefaultGame.ini` under
 * `[/Script/LoomaSceneSync.LoomaSceneSyncSettings]`.
 *
 * Read live through `Get()`: the CDO *is* the settings object, so an edit in the
 * panel is visible to the subsystem on the next read — no restart, and (for the
 * backend address) a reconnect happens by itself, see `OnSettingsChanged`.
 *
 * These knobs used to live on `ULoomaSceneSyncSubsystem` itself, so the older
 * section `[/Script/LoomaSceneSync.LoomaSceneSyncSubsystem]` is still read for any
 * key this section does not set — including its `BackendHost`, whose value is a valid
 * `BackendUrl` — so a project that pins its backend the old way keeps working. It is a
 * fallback, not a mirror: the first save from the panel writes the new section, which
 * then wins.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Looma Scene Sync"))
class LOOMASCENESYNC_API ULoomaSceneSyncSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    ULoomaSceneSyncSettings();

    /** The live settings. Never null — this is the class default object. */
    static const ULoomaSceneSyncSettings& Get();

    /**
     * Fired when the panel changes a setting. The subsystem listens and reconnects
     * when the backend address moves; editor-only, nothing broadcasts at runtime.
     */
    static FSimpleMulticastDelegate& OnSettingsChanged();

    /**
     * Where the backend is. Everything else is derived from it: the REST/asset base is
     * this, and the scene-sync hub is this with an ws/wss scheme plus `/ws/scene`.
     *
     * Both forms work:
     *   - `127.0.0.1:8000`   — a bare host:port, assumed http (what the plugin took before)
     *   - `https://host/api` — a full URL, including a path prefix, for a reverse proxy
     *                          or tunnel. Gives `wss://host/api/ws/scene`.
     *
     * Locally, prefer an explicit IPv4 address over "localhost": UE's WebSocket client
     * may resolve localhost to IPv6 (::1) while uvicorn listens on IPv4 only, which
     * shows up as endless "connect failed (retrying)". Editing this reconnects at once.
     */
    UPROPERTY(EditAnywhere, Config, Category = "Backend", meta = (DisplayName = "Backend URL"))
    FString BackendUrl = TEXT("http://127.0.0.1:8000");

    /**
     * Trim on every light's wire intensity. The units already match 1:1 (three.js is
     * in candela for point/spot and lux for directional, and so is UE), so this exists
     * only because the two renderers' exposure and tone mapping differ — see
     * `ALoomaSyncedActor::ApplyLight`.
     */
    UPROPERTY(EditAnywhere, Config, Category = "Rendering", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "4.0"))
    float LightIntensityScale = 1.0f;

    /**
     * Lift a `model`'s GLB so its bounding-box floor sits on the node origin, matching
     * the web client (frontend/src/scene/pivot.js). False restores the pre-v3 Unreal
     * behaviour — the GLB centred on the node origin, i.e. half-buried at floor level.
     */
    UPROPERTY(EditAnywhere, Config, Category = "Rendering", meta = (DisplayName = "Base-Align Models"))
    bool bBaseAlignModels = true;

    /**
     * Prefix a **web** client needs in front of `/static/...` — the Vite proxy path,
     * i.e. the backend's `LOOMA_PUBLIC_API_PREFIX`. Only used to fill in the `url` of
     * a `model` we spawn, since the browser cannot rebuild that path itself and the
     * hub will not invent it. Empty emits no url, which leaves a UE-originated spawn
     * invisible in the browser.
     */
    UPROPERTY(EditAnywhere, Config, Category = "Backend", meta = (DisplayName = "Web Asset Prefix"))
    FString WebAssetPrefix = TEXT("/api");

    // --- UObject ---
    virtual void PostInitProperties() override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    /** Pull anything still only set in the pre-settings-class .ini section. */
    void ImportLegacyConfig();
};
