#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaSceneSyncSubsystem.h"

/**
 * `Looma.Reconnect` / `Looma.Status` — the two things you want from a console when the
 * viewer shows an empty scene, in the editor console, in PIE, or in a packaged build —
 * plus `Looma.Login` / `Looma.Logout` / `Looma.Whoami` for the auth surface.
 *
 * The subsystem is per-GameInstance while a console command is global, so each command
 * resolves the subsystem when it runs: from the world the console handed us, else from
 * whichever game world is up.
 *
 * `Looma.Login` is a **testing affordance, not the login path**. A password typed at a
 * console is echoed into the log by the console itself — before this plugin sees a
 * character of it — and it stays in the command history for the session. Nothing here
 * can undo that. The real login is `ULoomaLoginAction` behind a text field that does
 * not echo; this exists so the REST surface can be exercised without building UI first.
 */
namespace
{
ULoomaSceneSyncSubsystem* FindLoomaSubsystem(UWorld* World)
{
    if (World)
    {
        if (UGameInstance* GameInstance = World->GetGameInstance())
        {
            if (ULoomaSceneSyncSubsystem* Subsystem = GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>())
            {
                return Subsystem;
            }
        }
    }
    // The editor console runs against the editor world, which has no game instance.
    if (GEngine)
    {
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
            {
                continue;
            }
            if (UGameInstance* GameInstance = Context.OwningGameInstance)
            {
                if (ULoomaSceneSyncSubsystem* Subsystem = GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>())
                {
                    return Subsystem;
                }
            }
        }
    }
    return nullptr;
}

FAutoConsoleCommandWithWorld GLoomaReconnectCommand(
    TEXT("Looma.Reconnect"),
    TEXT("Drop the Looma scene-sync socket and connect again, re-reading Project Settings > Plugins > Looma Scene Sync."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) {
        if (ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World))
        {
            Subsystem->Reconnect();
            return;
        }
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Looma Scene Sync is not running (no game instance) — nothing to reconnect. ")
            TEXT("Configured backend: %s"), *ULoomaSceneSyncSettings::Get().BackendUrl);
    }));

FAutoConsoleCommandWithWorld GLoomaStatusCommand(
    TEXT("Looma.Status"),
    TEXT("Log the Looma scene-sync connection: hub URL, REST base, socket state, backend auth state, ")
    TEXT("and a GET /health probe."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) {
        if (ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World))
        {
            Subsystem->LogConnectionStatus();
            return;
        }
        UE_LOG(LogLoomaSync, Display,
            TEXT("Looma Scene Sync: NOT RUNNING (no game instance) | configured backend %s"),
            *ULoomaSceneSyncSettings::Get().BackendUrl);
    }));

/**
 * One line for "the subsystem is not up", shared by the auth commands. They cannot
 * fall back to reporting the configured URL the way Status does — there is no identity
 * to report without a subsystem to hold one.
 */
void LogNoSubsystem(const TCHAR* Command)
{
    UE_LOG(LogLoomaSync, Warning,
        TEXT("%s: Looma Scene Sync is not running (no game instance), so there is no session to act on."),
        Command);
}

FAutoConsoleCommandWithWorldAndArgs GLoomaLoginCommand(
    TEXT("Looma.Login"),
    TEXT("Log in to the Looma backend: Looma.Login <username> <password>. Testing only — the console ")
    TEXT("echoes the password into the log."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        // Exactly two arguments, and no attempt to be clever about a password with a
        // space in it: the console splits on whitespace and there is no quoting
        // convention to lean on, so a usage line beats silently logging in as the
        // wrong thing. A password that cannot be typed here is a reason to use the
        // Blueprint node, which has no such limitation.
        if (Args.Num() != 2)
        {
            UE_LOG(LogLoomaSync, Display, TEXT("Usage: Looma.Login <username> <password>"));
            return;
        }
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Login"));
            return;
        }
        // The result is logged by RequestLogin itself (success and failure alike), so
        // this callback is only here because the call takes one. Nothing it receives is
        // safe to add: the identity is already in the log line, and the password is not
        // going anywhere near one.
        Subsystem->RequestLogin(Args[0], Args[1], nullptr);
    }));

FAutoConsoleCommandWithWorldAndArgs GLoomaLogoutCommand(
    TEXT("Looma.Logout"),
    TEXT("Forget the Looma session token and ask the backend to revoke it."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        if (ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World))
        {
            Subsystem->Logout();
            return;
        }
        LogNoSubsystem(TEXT("Looma.Logout"));
    }));

FAutoConsoleCommandWithWorldAndArgs GLoomaWhoamiCommand(
    TEXT("Looma.Whoami"),
    TEXT("Log the current Looma identity, then re-ask the backend (GET /auth/me) when a token is held."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Whoami"));
            return;
        }
        UE_LOG(LogLoomaSync, Display, TEXT("Looma identity: %s"), *Subsystem->GetIdentityText());
        // Refresh only when there is a token to check. Without one the answer is a
        // freshly-minted random guest name that matches nothing in the room roster, so
        // asking would print a distraction rather than an answer — see RefreshIdentity.
        if (Subsystem->HasAuthToken())
        {
            Subsystem->RefreshIdentity();
        }
    }));
} // namespace
