#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"

/**
 * The console surface, which as of HAM-239 is self-sufficient: every id and name these
 * commands accept can be discovered from the same console, with no browser tab beside
 * it. Eight groups, and the shape of each is a fact about the wire rather than a choice
 * of style.
 *
 *   Looma.Status / Looma.Reconnect            what the socket is doing, and do it again
 *   Looma.Login / Logout / Whoami             the auth surface
 *   Looma.Scene [id | "name"]                 where this client is looking; a MESSAGE
 *   Looma.Performance [id [cue]]              which workspace; a RECONNECT
 *   Looma.Cue [index]                         where on the running order
 *   Looma.Scenes / Looma.Performances         what there is to name in the three above
 *   Looma.Select / Deselect / Selection       what this client has selected
 *   Looma.Room                                who else is in the room, and what
 *                                             each of them holds
 *
 * The one distinction worth carrying into all of them: changing SCENE is a message and
 * changing PERFORMANCE is a reconnect, because a socket's performance is fixed at
 * `hello` and never changes for its life. Everything else — why `Looma.Scene` refuses
 * while disconnected and `Looma.Performance` does not, why a cue is an `openScene` and
 * not a rejoin, why a two-argument switch has to park its cue — follows from that.
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

/**
 * A whole decimal number, or false — the strict parse `Looma.Cue` needs.
 *
 * FCString::Atoi is not usable here: it answers 0 for anything it cannot read, so a
 * mistyped cue number would silently fire the opening cue. A leading '-' is accepted so
 * that a negative reaches the range message, which names the valid cues, rather than
 * being turned away as gibberish — "there is no cue -1, this one has 0 to 4" is the
 * answer somebody wants, and "that is not a number" is not.
 */
bool ParseIndexArgument(const FString& Text, int32& OutIndex)
{
    const int32 First = (!Text.IsEmpty() && Text[0] == TEXT('-')) ? 1 : 0;
    if (Text.Len() <= First)
    {
        return false;
    }
    for (int32 Index = First; Index < Text.Len(); ++Index)
    {
        if (!FChar::IsDigit(Text[Index]))
        {
            return false;
        }
    }
    OutIndex = FCString::Atoi(*Text);
    return true;
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
 * `Looma.Scene [sceneId | "name"]` — which saved scene this client is on, and how to
 * move it.
 *
 * An argument may be either the scene's id or its name; the name is resolved against
 * `GET /scenes` before anything is sent, so an unquoted multi-word name is rejoined
 * here and a miss is answered with the catalogue rather than a bare refusal from the
 * hub. See OpenSceneByNameOrId for why every open now costs that lookup.
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
    TEXT("Open a saved scene for this client by id or by name: Looma.Scene <sceneId> | ")
    TEXT("Looma.Scene \"<name>\". With no arguments, log the active scene's id and name."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
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
        // JOINED, not refused. `Looma.Scene "Costume Test"` already arrives as one
        // argument — the console's tokenizer consumes a quoted run and strips the quotes
        // — so the multi-argument case is somebody who did not quote, which on this
        // backend is every scene there is: `_slugify_scene` puts the hyphens in the id
        // and leaves the spaces in the name, so refusing would be refusing the ordinary
        // case in order to teach a convention. Rejoining with single spaces reconstructs
        // anything the console's whitespace collapsing did not destroy, and a stray
        // extra token merely makes the lookup miss — which answers with the catalogue
        // and the quoting hint anyway, so the lesson still gets taught, by the path that
        // also works. The quoted form remains the exact one, for a name with runs of
        // spaces that this cannot rebuild.
        //
        // Which is why there is no arity check any more: with a join, every argument
        // count from one upwards is a legal lookup, and a usage line would be refusing
        // input this understands.
        Subsystem->OpenSceneByNameOrId(FString::Join(Args, TEXT(" ")));
        // Nothing is printed here about the outcome, because none is known yet: the
        // resolver says what it matched, and what actually happened then arrives as a
        // `scene` frame ("Scene 'x': N node(s) applied") or as a refusal from
        // HandleSceneError. A cheerful line at this point would be the one entry in the
        // log that cannot be trusted.
    }));

/**
 * `Looma.Scenes` — every scene this identity may open, id and name, active one marked.
 *
 * The command the user actually asked for: `Looma.Scene <name-or-id>` accepts two kinds
 * of string and, before this, nothing in the client would tell them either. Answering
 * "what are the ids" by alt-tabbing to a browser is not answering it.
 *
 * No arguments of any kind, not even a filter. A filter is a second matching rule to
 * keep in step with the one in OpenSceneByNameOrId, and on six rows it would be
 * answering a question nobody has yet asked.
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaScenesCommand(
    TEXT("Looma.Scenes"),
    TEXT("List every scene this client may open: id, name, and which one is active."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Scenes"));
            return;
        }
        // Arguments ignored rather than refused. There is nothing they could mean, the
        // command is harmless, and a usage line for a command that takes nothing is a
        // paragraph explaining a blank.
        //
        // No connectivity check either, and the asymmetry with `Looma.Scene <id>` is the
        // point: this reads the catalogue over HTTP and never touches the socket, so it
        // answers perfectly well while disconnected — which is exactly when somebody is
        // working out where to reconnect to.
        Subsystem->LogScenes();
    }));

/**
 * `Looma.Performance [performance-id [cue-index]]` — which workspace this socket is in,
 * how to move it, and how to land somewhere particular on arrival.
 *
 * With an id it RECONNECTS, and there is no gentler option: a socket's performance is
 * chosen once, from the `hello`, and never changes for the life of that connection
 * (docs/scene-format.md, "Performance selection (HAM-197)"). That is the asymmetry with
 * `Looma.Scene`, which sends a message and keeps its socket — and it is why this command
 * does NOT test IsSyncConnected() first the way that one does. A switch typed while the
 * socket is down is not lost, it is the whole repair: it records the intent and starts
 * the connection that carries it.
 *
 * With an id AND an index it does both, in the only order that works: the cue is parked
 * and fired when the new socket is confirmed in a performance, because the socket that
 * must carry the `openScene` does not exist yet and a send before it opens is dropped
 * without trace. See SwitchPerformanceToCue for what drops a parked cue and why landing
 * somewhere unasked-for is one of those things.
 *
 * With no arguments it reports, and reports the pending switch first when there is one.
 * That order is not cosmetic — during a switch the "current" performance is the room
 * being left, and it looks perfectly valid.
 *
 * The prose lives here rather than behind a subsystem Log... method, unlike
 * `Looma.Scene`. That one had to sit in the subsystem because a scene's name costs a
 * request needing GetRestBase and ApplyAuthHeader; this needs nothing the getters do
 * not already hand out, and `Looma.Selection` is the precedent for a command that
 * formats a public getter itself.
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaPerformanceCommand(
    TEXT("Looma.Performance"),
    TEXT("Switch this client to a performance: Looma.Performance <performance-id> (a reconnect). ")
    TEXT("With no arguments, log the performance this client is in."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        // An id, an id and a cue index, or neither. Never a name to rejoin: unlike
        // `Looma.Scene`, both arguments here are slugs and numbers, so a third token is
        // a typo rather than a value that needed quoting.
        if (Args.Num() > 2)
        {
            UE_LOG(LogLoomaSync, Display,
                TEXT("Usage: Looma.Performance [<performance-id> [<cue-index>]]  (with no ")
                TEXT("arguments, reports the current performance)"));
            return;
        }
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Performance"));
            return;
        }
        if (Args.Num() >= 1)
        {
            // No local check on the id, and no connectivity check either. The hub owns
            // both questions: it re-resolves identity and re-runs the visibility gate,
            // and it answers a refusal with a bare 4403/4404 close that stops the retry
            // loop and explains itself. Nothing said from here could improve on that.
            if (Args.Num() == 1)
            {
                Subsystem->SwitchPerformance(Args[0]);
                return;
            }
            int32 CueIndex = 0;
            if (!ParseIndexArgument(Args[1], CueIndex))
            {
                // Checked before the switch, so a mistyped cue number costs nobody a
                // reconnect. The same strict parse `Looma.Cue` uses, and for the same
                // reason: Atoi reads anything it cannot parse as 0, which here would
                // switch performance AND open the opening cue of it.
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("Looma.Performance: '%s' is not a cue number, so nothing was switched. ")
                    TEXT("Usage: Looma.Performance <performance-id> <cue-index>, 0-based."),
                    *Args[1]);
                return;
            }
            Subsystem->SwitchPerformanceToCue(Args[0], CueIndex);
            return;
        }
        const FString Pending = Subsystem->GetPendingPerformanceId();
        if (!Pending.IsEmpty())
        {
            // Before anything about the current performance, because until the new
            // socket's `scene` frame lands the "current" one is the room being left —
            // nothing clears it on a drop, so it reads as valid while being one socket
            // out of date.
            UE_LOG(LogLoomaSync, Display,
                TEXT("Switching to performance '%s' — waiting for the new socket's `scene` frame. ")
                TEXT("Until it lands, what follows is the performance being left."),
                *Pending);
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
 * `Looma.Performances` — every workspace this identity may see, with our role in each.
 *
 * The other half of `Looma.Scenes`, and the one that makes `Looma.Performance <id>`
 * usable: a switch takes an id, and this is where the ids come from. It prints `role`
 * as well, because a workspace this caller may only watch and one it may reshape are
 * different answers to "should I switch to this".
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaPerformancesCommand(
    TEXT("Looma.Performances"),
    TEXT("List every performance (workspace) this client may see: id, name, our role, and which ")
    TEXT("one this socket is in."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Performances"));
            return;
        }
        // Arguments ignored and no connectivity check, for the same two reasons as
        // `Looma.Scenes` — and the second matters more here, since the reason to read
        // this list is often that a switch was refused and the socket is stopped.
        Subsystem->LogPerformances();
    }));

/**
 * `Looma.Cue [index]` — step along the current performance's running order.
 *
 * The command this whole arc was for. A cue is a point on the chronology, so firing one
 * is an `openScene` within the performance already connected — no reconnect, unlike
 * `Looma.Performance <id>`, and that difference is why the two are not one command.
 *
 * The index is **0-based**, matching the backend's own `order_idx` numbering rather than
 * the 1-based counting a paper cue sheet uses. One convention, chosen once: a console
 * that renumbered would disagree with every other tool over the same rail.
 *
 * Arguments are not joined here, unlike `Looma.Scene`, and the asymmetry is the
 * argument's shape rather than a change of mind: a scene name has spaces in it and an
 * index cannot, so a second token is a typo with no reading that makes it one number.
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaCueCommand(
    TEXT("Looma.Cue"),
    TEXT("Open the scene at a position on the current performance's running order: ")
    TEXT("Looma.Cue <index>, 0-based. With no arguments, print the running order."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        if (Args.Num() > 1)
        {
            UE_LOG(LogLoomaSync, Display,
                TEXT("Usage: Looma.Cue <index>  (0-based; with no arguments, prints the running ")
                TEXT("order)"));
            return;
        }
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            LogNoSubsystem(TEXT("Looma.Cue"));
            return;
        }
        if (Args.Num() == 0)
        {
            // No connectivity check: the running order is a catalogue read over HTTP,
            // so it answers while the socket is down — and being unable to see the rail
            // because the socket dropped would be a strange way to find out.
            Subsystem->LogCues();
            return;
        }
        int32 Index = 0;
        if (!ParseIndexArgument(Args[0], Index))
        {
            // Refused rather than passed to Atoi, which reads "two" as 0 and would fire
            // the opening cue on a typo. A cue number is the one argument in this file
            // where a wrong-but-plausible reading has an audience.
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Looma.Cue: '%s' is not a cue number. Usage: Looma.Cue <index>, 0-based."),
                *Args[0]);
            return;
        }
        if (!Subsystem->IsSyncConnected())
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Looma.Cue: not fired — the scene-sync socket is not connected, and the ")
                TEXT("`openScene` a cue resolves to would be dropped rather than queued. ")
                TEXT("`Looma.Reconnect` first. `Looma.Cue` with no argument still reads the rail."));
            return;
        }
        Subsystem->OpenCue(Index);
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

/**
 * `Looma.Room` — the other half of `Looma.Selection`: what everyone *else* has, from
 * the hub's `clients` roster.
 *
 * Also the only way this client can read its own room name. `Looma.Whoami` cannot
 * answer that for a guest — `GET /auth/me` mints a fresh random `Guest-xxxxxx` per
 * call — so the self entry here, marked with a `*`, is the one place the name other
 * people see for us appears.
 */
FAutoConsoleCommandWithWorldAndArgs GLoomaRoomCommand(
    TEXT("Looma.Room"),
    TEXT("Log who else is in the room from the hub's roster: id, name, kind, role, colour and ")
    TEXT("selection, with this client's own entry marked."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
        ULoomaSceneSyncSubsystem* Subsystem = FindLoomaSubsystem(World);
        if (!Subsystem)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Looma.Room: Looma Scene Sync is not running (no game instance)."));
            return;
        }
        Subsystem->LogRoom();
    }));
} // namespace
