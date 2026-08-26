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

    /**
     * The name to *suggest* for this client in the room roster, when it is connected
     * as a guest. Empty — the default — suggests nothing, and the hub falls back to
     * `Guest-xxxxxx` built from the first six characters of our clientId.
     *
     * A suggestion, never a claim. The hub clamps it to 32 characters, strips control
     * characters, rejects it outright if nothing survives that, and — when accounts
     * are enabled — rejects it if its normalized form collides with a **registered**
     * username: a guest may not wear the name of an account that exists, whether or
     * not it can prove it (docs/scene-format.md, "Identity — the `hello` message and
     * guest naming"). So this cannot be used to impersonate anyone, and there is no
     * point validating it here — the hub is the authority and the roster is where it
     * says what it made of the suggestion.
     *
     * Ignored entirely while logged in. A session resolves the identity and the
     * suggestion is never consulted, so this renames a *guest*, never an account.
     *
     * Editing it applies at once **while connected as a guest**: the name rides in the
     * `hello` and nothing renames a socket already up, so the subsystem reconnects to
     * suggest the new one. While logged in it deliberately does nothing — the hub takes
     * the name from the session and would ignore the suggestion, so a reconnect could
     * not change anything while still costing every other client in the room a leave
     * and a join. The edit is not lost either way: the next guest connection picks it
     * up, and logging out is one.
     *
     * Unlike the session token, this belongs in Config: it is a preference, not a
     * secret, and having it in `DefaultGame.ini` is exactly right for a viewer build
     * that should come up with a sensible name on a headset nobody types on.
     */
    UPROPERTY(EditAnywhere, Config, Category = "Identity", meta = (DisplayName = "Guest Display Name"))
    FString GuestDisplayName;

    // --- UObject ---
    virtual void PostInitProperties() override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    /** Pull anything still only set in the pre-settings-class .ini section. */
    void ImportLegacyConfig();
};
