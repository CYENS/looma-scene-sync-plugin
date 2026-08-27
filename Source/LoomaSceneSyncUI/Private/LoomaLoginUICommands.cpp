#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "LoomaLoginUI.h"
#include "LoomaSceneSyncLog.h"

/**
 * `Looma.ShowLogin` / `Looma.HideLogin` (HAM-182).
 *
 * The plugin cannot put a widget on screen by itself — something has to call
 * `Show Login UI`, and until now that something had to be a Blueprint node wired by
 * hand. That made the first test of this form depend on a step that has nothing to do
 * with the form, and "nothing appeared" then has one more candidate cause than it should.
 *
 * These are the same front door as the Blueprint nodes, not a second implementation:
 * both call `ULoomaLoginUI`, so testing through the console tests the shipping path.
 * `Looma.Reconnect` and `Looma.Status` set the precedent — everything in this plugin has
 * been reachable from the console, which is why it has been testable at all.
 *
 * Deliberately in the UI module. The core module's `LoomaSceneSyncCommands.cpp` must not
 * grow a command for a widget it cannot see, and these must disappear along with the
 * module when a consumer drops it.
 */

namespace
{
/** The world a console command should act on, or null with a reason logged. */
UWorld* CommandWorld(UWorld* World)
{
    if (!World)
    {
        UE_LOG(LogLoomaSync, Warning, TEXT("Login UI: no world (run this in PIE)"));
        return nullptr;
    }
    return World;
}
} // namespace

FAutoConsoleCommandWithWorld GLoomaShowLoginCommand(
    TEXT("Looma.ShowLogin"),
    TEXT("Show the Looma login form. Logs which state it found."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) {
        if (UWorld* W = CommandWorld(World))
        {
            // The world is the world-context object; GetWorldFromContextObject accepts a
            // UWorld directly, so there is nothing to look up.
            ULoomaLoginUI::ShowLoginUI(W);
        }
    }));

FAutoConsoleCommandWithWorld GLoomaHideLoginCommand(
    TEXT("Looma.HideLogin"),
    TEXT("Take the Looma login form off the viewport."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) {
        if (UWorld* W = CommandWorld(World))
        {
            ULoomaLoginUI::HideLoginUI(W);
            UE_LOG(LogLoomaSync, Display, TEXT("Login UI: hidden"));
        }
    }));
