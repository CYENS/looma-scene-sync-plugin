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
 * plus `Looma.Login` / `Looma.Logout` / `Looma.Whoami` for the auth surface and
 * `Looma.Scene` / `Looma.Performance` for which scene this client is looking at and
 * which workspace it is looking at it in.
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
 * `Looma.Scene [sceneId]` — which saved scene this client is on, and how to move it.
 *
 * With an id it sends `openScene`, which subscribes THIS client and moves nobody else
 * in the room (docs/scene-format.md, "Choosing a scene (HAM-185)"). Note the asymmetry
 * the contract draws and this command inherits: switching PERFORMANCE is a reconnect,
 * switching scene is a message.
 *
 * It checks the socket first, which the auth commands deliberately do not — they act
 * over HTTP and have their own answer when the backend is unreachable. An `openScene`
 * typed while the socket is down is not merely ineffective, it is *invisible*: SendJson
 * returns silently on a dead socket, and the contract says the same of a send before
 * the socket is open. So this says no rather than looking like it worked. It does not
 * park the id for the next connect — the web client does exactly that for its `#/s/<id>`
 * deep links, and it is a real feature with its own ordering rules, not a line to slip
 * into a console command.
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaSceneCommand(
    TEXT("Looma.Scene"),
    TEXT("Open a saved scene for this client: Looma.Scene <sceneId>. With no arguments, log the ")
    TEXT("active scene's id and name."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        // One id, or none. A scene id is a slug and cannot contain a space
        // (`_slugify_scene`, backend/app/scenes.py), so a second argument is a typo
        // rather than a value that needed quoting — the same reasoning as
        // `Looma.Login`'s argument count, and the opposite of `Looma.Select`, where a
        // list of ids is the whole point.
        if (Args.Num() > 1)
        {
            UE_LOG(LogLoomaSync, Display,
                TEXT("Usage: Looma.Scene <sceneId>  (with no arguments, reports the active scene)"));
            return;
        }
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Scene"));
            return;
        }
        if (Args.Num() == 0)
        {
            // Two lines, the second arriving later — see LogActiveScene for why the id
            // is not made to wait for the name.
            Subsystem->LogActiveScene();
            return;
        }
        if (!Subsystem->IsSyncConnected())
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Looma.Scene: not sent — the scene-sync socket is not connected, and `openScene` ")
                TEXT("on a closed socket is dropped, not queued. `Looma.Status` for why, ")
                TEXT("`Looma.Reconnect` to retry now, then type this again."));
            return;
        }
        Subsystem->OpenScene(Args[0]);
        // Nothing is printed here about the outcome, because none is known yet: what
        // happened arrives as a `scene` frame ("Scene 'x': N node(s) applied") or as a
        // refusal from HandleSceneError. A cheerful line at this point would be the one
        // entry in the log that cannot be trusted.
    }));

/**
 * `Looma.Performance` — which workspace this socket is in.
 *
 * Read-only, and that is a fact about the wire rather than an unfinished command. The
 * hub resolves the workspace once, from the `hello`, and there is no message that moves
 * a socket already up: switching PERFORMANCE is a reconnect where switching SCENE is a
 * message. So an argument gets a usage line that names what is missing instead of being
 * quietly ignored — a `Looma.Performance <id>` that accepted an id and did nothing is
 * the one outcome worse than not having the command at all.
 *
 * The prose lives here rather than behind a subsystem Log... method, unlike
 * `Looma.Scene`. That one had to sit in the subsystem because a scene's name costs a
 * request needing GetRestBase and ApplyAuthHeader; this needs nothing the getters do
 * not already hand out, and `Looma.Selection` is the precedent for a command that
 * formats a public getter itself.
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaPerformanceCommand(
    TEXT("Looma.Performance"),
    TEXT("Log the performance (workspace) this client is in: its id, name and visibility."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        if (Args.Num() != 0)
        {
            UE_LOG(LogLoomaSync, Display,
                TEXT("Usage: Looma.Performance  (no arguments — reports the current performance). ")
                TEXT("Switching with `Looma.Performance <performance-id>` is not implemented yet: it ")
                TEXT("is a reconnect and not a message, so it arrives with `hello.performanceId` and ")
                TEXT("the terminal close codes that go with it."));
            return;
        }
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Performance"));
            return;
        }
        const FLoomaPerformance Performance = Subsystem->GetActivePerformance();
        if (Performance.Id.IsEmpty())
        {
            // Ignorance, not a property of any workspace: the hub cannot put us
            // somewhere without naming its id, so an empty one means no `scene` frame
            // has arrived — the only carrier there is.
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Current performance: unknown — no `scene` frame has named one%s."),
                Subsystem->IsSyncConnected()
                    ? TEXT(" yet")
                    : TEXT(", because the socket is not connected (`Looma.Status` says why)"));
            return;
        }
        // Never empty in practice — the hub defaults it to `public` — but printing bare
        // parentheses if it ever were would read as a bug in this line rather than as a
        // field the frame left out.
        const TCHAR* Visibility = Performance.Visibility.IsEmpty()
            ? TEXT("visibility unknown")
            : *Performance.Visibility;
        if (Performance.Name.IsEmpty())
        {
            // A different state from the one above, and worth its own branch: this is
            // the hub telling us the row behind the id is gone, not the hub failing to
            // tell us anything. The socket is still in it and still works, so the line
            // says where we are before it says what is missing.
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Current performance '%s' (%s) — the hub sent no name, which is what it sends ")
                TEXT("for a row that has been deleted. The id is still where this socket is."),
                *Performance.Id, Visibility);
            return;
        }
        UE_LOG(LogLoomaSync, Display, TEXT("Current performance '%s' — '%s' (%s)"),
            *Performance.Id, *Performance.Name, Visibility);
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
