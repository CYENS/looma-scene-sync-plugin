#include "LoomaLoginUI.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSubsystem.h"
#include "SLoomaLoginWidget.h"
#include "Widgets/Layout/SBox.h"

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
TWeakPtr<SWidget> GLoginRoot;

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
    // Every return from here says so. An earlier version returned silently from both
    // early exits below, which made `Looma.ShowLogin` indistinguishable from a broken
    // widget in exactly the case the logging was added to diagnose — a form that does not
    // appear and a command that says nothing leave a reader with no way to tell which
    // happened. Idempotence and "there is nowhere to draw" are both answers, so both are
    // reported.
    if (GLoginRoot.IsValid())
    {
        UE_LOG(LogLoomaSync, Display,
            TEXT("Show Login UI: already showing (this call is a no-op). Use Looma.HideLogin first ")
            TEXT("if you meant to recreate it."));
        return;
    }

    UGameViewportClient* Viewport = ViewportFor(WorldContextObject);
    if (!Viewport)
    {
        // Display, not Verbose. A commandlet or dedicated server genuinely has no viewport
        // and that is not an error — but the overwhelmingly common cause is a human
        // typing this into the editor console with no PIE session running, and a silent
        // no-op is the worst possible answer to give them.
        UE_LOG(LogLoomaSync, Display,
            TEXT("Show Login UI: no game viewport, so nothing was shown. Start PIE (or a game) — ")
            TEXT("there is no viewport to draw on in the editor alone."));
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

    // Anchored top-left at a fixed width, not handed to the viewport bare.
    // AddViewportWidgetContent drops the widget into a full-screen overlay whose slot
    // fills, and SBorder fills too — so an unwrapped form stretches its background over
    // the entire viewport with three controls stranded in the middle of it. Looks like a
    // bug, is one, and costs one SBox to avoid.
    const TSharedRef<SLoomaLoginWidget> Widget = SNew(SLoomaLoginWidget, Subsystem);
    const TSharedRef<SWidget> Root =
        SNew(SBox)
        .HAlign(HAlign_Left)
        .VAlign(VAlign_Top)
        .Padding(24.0f)
        [
            SNew(SBox).WidthOverride(280.0f)[Widget]
        ];

    Viewport->AddViewportWidgetContent(Root, ZOrder);
    GLoginRoot = Root;

    // Say what it decided. "Show Login UI did nothing visible" is otherwise
    // indistinguishable from three different causes — no subsystem, an auth state still
    // unknown, or a backend that wants no login — and the form's whole job is to render
    // nothing in two of them.
    if (Subsystem)
    {
        UE_LOG(LogLoomaSync, Display,
            TEXT("Show Login UI: %s"),
            !Subsystem->IsAuthStateKnown()
                ? TEXT("auth state not known yet — the form stays hidden until /health answers")
                : (!Subsystem->IsAuthEnabled()
                    ? TEXT("this backend has auth disabled — the form stays hidden")
                    : (Subsystem->HasAuthToken()
                        ? TEXT("already signed in — showing the identity and a log-out button")
                        : TEXT("auth required and no session — showing the login form"))));
    }
}

void ULoomaLoginUI::HideLoginUI(UObject* WorldContextObject)
{
    const TSharedPtr<SWidget> Root = GLoginRoot.Pin();
    if (!Root.IsValid())
    {
        return;
    }
    if (UGameViewportClient* Viewport = ViewportFor(WorldContextObject))
    {
        // The outer wrapper, which is what was added — RemoveViewportWidgetContent
        // matches on the exact widget it was given, so passing the inner form would
        // silently remove nothing.
        Viewport->RemoveViewportWidgetContent(Root.ToSharedRef());
    }
    // Reset even if the viewport had already gone: holding a weak handle to a widget
    // nothing will ever remove would make a later Show a silent no-op.
    GLoginRoot.Reset();
}

bool ULoomaLoginUI::IsLoginUIShowing()
{
    return GLoginRoot.IsValid();
}
