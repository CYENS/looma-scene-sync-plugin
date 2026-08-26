#include "LoomaLoginUI.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSubsystem.h"
#include "SLoomaLoginWidget.h"

namespace
{
/**
 * The one form on screen, if there is one.
 *
 * A file-static is not the shape I would choose if this were a general widget manager,
 * and it is the right one here: `RemoveViewportWidgetContent` needs the exact
 * `TSharedRef` it was given, so something has to remember it, and a
 * `UBlueprintFunctionLibrary` has nowhere to put state. There is one viewport and one
 * login form, so one slot is the whole requirement — a map keyed by world would be
 * machinery for a case that does not exist.
 *
 * Weak: the viewport owns the widget, so this must never be what keeps it alive. A
 * viewport torn down under us leaves this expired rather than dangling, and every read
 * below pins before use.
 *
 * **Known limitation, stated rather than discovered:** one slot means one form per
 * *process*, not per world. Under "Play As Client" with two PIE windows, the second
 * `Show Login UI` is a silent no-op. That is a real hole and it is deliberately not
 * plugged here — a world-keyed map is machinery for a case this widget does not serve
 * (two accounts logging in from one editor process share a single game-instance
 * subsystem anyway, so a second form would be showing the first one's session). A
 * consumer who needs per-window login is the consumer who should be building their own
 * widget against the Blueprint API.
 */
TWeakPtr<SLoomaLoginWidget> GLoginWidget;

/** The viewport for a Blueprint world-context object, or null in a headless run. */
UGameViewportClient* ViewportFor(const UObject* WorldContextObject)
{
    if (!GEngine || !WorldContextObject)
    {
        return nullptr;
    }
    const UWorld* World = GEngine->GetWorldFromContextObject(
        WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    return World ? World->GetGameViewport() : nullptr;
}
} // namespace

void ULoomaLoginUI::ShowLoginUI(UObject* WorldContextObject, int32 ZOrder)
{
    if (GLoginWidget.IsValid())
    {
        return; // already up — see the header on why this is idempotent rather than additive
    }

    UGameViewportClient* Viewport = ViewportFor(WorldContextObject);
    if (!Viewport)
    {
        // Not a failure worth shouting about: a commandlet or a dedicated server has no
        // viewport, and this module being compiled into such a build is a packaging
        // decision rather than a mistake to assert on.
        UE_LOG(LogLoomaSync, Verbose, TEXT("Show Login UI: no game viewport; nothing shown"));
        return;
    }

    // The subsystem lives on the game instance, and the widget holds it weakly. Null is
    // survivable — the widget simply stays collapsed — but it means the plugin is not
    // running, which is worth one line, because a login form that never appears is
    // otherwise indistinguishable from a backend that wants no login.
    ULoomaSceneSyncSubsystem* Subsystem = nullptr;
    if (const UWorld* World = Viewport->GetWorld())
    {
        if (UGameInstance* GameInstance = World->GetGameInstance())
        {
            Subsystem = GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>();
        }
    }
    if (!Subsystem)
    {
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Show Login UI: no LoomaSceneSync subsystem on this game instance — the form will ")
            TEXT("stay hidden. Is the plugin enabled and is this a game world?"));
    }

    const TSharedRef<SLoomaLoginWidget> Widget = SNew(SLoomaLoginWidget, Subsystem);
    Viewport->AddViewportWidgetContent(Widget, ZOrder);
    GLoginWidget = Widget;
}

void ULoomaLoginUI::HideLoginUI(UObject* WorldContextObject)
{
    const TSharedPtr<SLoomaLoginWidget> Widget = GLoginWidget.Pin();
    if (!Widget.IsValid())
    {
        return;
    }
    if (UGameViewportClient* Viewport = ViewportFor(WorldContextObject))
    {
        Viewport->RemoveViewportWidgetContent(Widget.ToSharedRef());
    }
    // Reset even if the viewport had already gone: holding a weak handle to a widget
    // nothing will ever remove would make a later Show a silent no-op.
    GLoginWidget.Reset();
}

bool ULoomaLoginUI::IsLoginUIShowing()
{
    return GLoginWidget.IsValid();
}
