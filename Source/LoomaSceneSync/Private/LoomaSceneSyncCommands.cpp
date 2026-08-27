#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"

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
/**
 * `Looma.Select <nodeId...>` / `Looma.Deselect [nodeId...]` / `Looma.Selection` — the
 * only way in as of step 1, since nothing hooks the editor's own selection yet.
 *
 * The subsystem's API takes actors, not ids, so these resolve each id through
 * FindSyncedActor first. That is deliberately the only id-to-actor door: an id this
 * client does not hold cannot be selected, and saying so is more use than silently
 * filing a claim on a node we could not draw. Note the asymmetry with the *inbound*
 * rule, which is the opposite by design — an unknown id arriving from the hub is kept,
 * not dropped, because a selection legitimately races the spawn that created its node.
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaSelectCommand(
    TEXT("Looma.Select"),
    TEXT("Replace the local selection and report it to the room: Looma.Select <nodeId> [nodeId...]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        if (Args.Num() == 0)
        {
            UE_LOG(LogLoomaSync, Display,
                TEXT("Usage: Looma.Select <nodeId> [nodeId...]  (Looma.Deselect with no arguments clears)"));
            return;
        }
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Looma.Select: Looma Scene Sync is not running (no game instance)."));
            return;
        }
        TArray<ALoomaSyncedActor*> Actors;
        for (const FString& NodeId : Args)
        {
            if (ALoomaSyncedActor* Actor = Subsystem->FindSyncedActor(NodeId))
            {
                Actors.Add(Actor);
            }
            else
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("Looma.Select: no node '%s' on this client"), *NodeId);
            }
        }
        // Whole-set replace, mirroring the wire — Looma.Select is not additive. The
        // resolved subset is applied even if some ids were unknown: the alternative,
        // refusing the whole command, would make one typo undo a selection the caller
        // had already built up.
        Subsystem->SetLocalSelection(Actors);
        UE_LOG(LogLoomaSync, Display, TEXT("Selected %d of %d requested node(s)"), Actors.Num(), Args.Num());
    }));

FAutoConsoleCommandWithWorldAndArgs GLoomaDeselectCommand(
    TEXT("Looma.Deselect"),
    TEXT("Deselect nodes and report it: Looma.Deselect [nodeId...]. With no arguments, clears the whole ")
    TEXT("selection, which is what removes this client's borders elsewhere."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Looma.Deselect: Looma Scene Sync is not running (no game instance)."));
            return;
        }
        if (Args.Num() == 0)
        {
            Subsystem->ClearSelection();
            UE_LOG(LogLoomaSync, Display, TEXT("Selection cleared"));
            return;
        }
        for (const FString& NodeId : Args)
        {
            // Null is fine to pass on — DeselectNode ignores it — but naming the miss is
            // the useful half, since "nothing happened" and "that node is not here" look
            // identical from a console otherwise.
            ALoomaSyncedActor* Actor = Subsystem->FindSyncedActor(NodeId);
            if (!Actor)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("Looma.Deselect: no node '%s' on this client"), *NodeId);
                continue;
            }
            Subsystem->DeselectNode(Actor);
        }
        UE_LOG(LogLoomaSync, Display, TEXT("Selection is now %d node(s)"),
            Subsystem->GetLocalSelectionIds().Num());
    }));

FAutoConsoleCommandWithWorldAndArgs GLoomaSelectionCommand(
    TEXT("Looma.Selection"),
    TEXT("Log this client's local selection — the ids the next `selection` message would carry."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Looma.Selection: Looma Scene Sync is not running (no game instance)."));
            return;
        }
        const TArray<FString> Ids = Subsystem->GetLocalSelectionIds();
        // Said explicitly rather than printed as an empty list, because an empty
        // selection is a real state with a real message behind it, not an absence.
        UE_LOG(LogLoomaSync, Display, TEXT("Local selection: %s"),
            Ids.Num() == 0 ? TEXT("<empty>") : *FString::Join(Ids, TEXT(", ")));
    }));
} // namespace
