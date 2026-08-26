#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LoomaLoginUI.generated.h"

/**
 * The front door to the plugin's bare-bones login form (HAM-182) — two Blueprint calls,
 * and the only public surface this module has.
 *
 * The widget itself lives in `Private/`, on purpose. This module ships a *working
 * starting point*, not a base class: anyone who wants a designed login screen should
 * build their own against the `Looma|Auth` surface on `ULoomaSceneSyncSubsystem`
 * (`IsAuthEnabled`, `GetIdentity`, `Login`, `Logout`, `OnAuthStateChanged`,
 * `OnIdentityChanged`) rather than inherit and fight this one. Shipping an asset or a
 * base class invites editing it; shipping an API invites replacing it.
 *
 * Showing it is safe whatever the backend turns out to want: the widget collapses itself
 * while the auth state is unknown and while auth is disabled, so a consumer can call
 * `Show Login UI` once at startup without first working out whether a login is needed.
 * That check is the widget's job and it is the one thing here most worth not
 * reimplementing — see `SLoomaLoginWidget`'s note on why "undecided" must not read as
 * "no login needed".
 */
UCLASS()
class LOOMASCENESYNCUI_API ULoomaLoginUI : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Put the login form on the viewport. Idempotent — calling it twice does not stack a
     * second copy, which matters because the natural place to call it is BeginPlay and
     * the natural way to get it wrong is a level that reloads.
     *
     * Does nothing in a build with no game viewport (a commandlet, a server), rather
     * than asserting: a headless run has no business showing a form, and this module
     * being enabled there is a packaging choice, not an error to crash on.
     */
    UFUNCTION(BlueprintCallable, Category = "Looma|Auth",
        meta = (WorldContext = "WorldContextObject", DisplayName = "Show Login UI"))
    static void ShowLoginUI(UObject* WorldContextObject, int32 ZOrder = 100);

    /** Take it off the viewport. Safe to call when it is not showing. */
    UFUNCTION(BlueprintCallable, Category = "Looma|Auth",
        meta = (WorldContext = "WorldContextObject", DisplayName = "Hide Login UI"))
    static void HideLoginUI(UObject* WorldContextObject);

    /** Whether the form is currently on the viewport. */
    UFUNCTION(BlueprintPure, Category = "Looma|Auth", meta = (DisplayName = "Is Login UI Showing"))
    static bool IsLoginUIShowing();
};
