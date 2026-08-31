#include "LoomaSceneSyncSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IWebSocket.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "LoomaAuthTypes.h"
#include "LoomaGenerationHandle.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaSyncedActor.h"
#include "LoomaWireConvert.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_EDITOR
#include "Editor.h"    // GEditor
#include "Selection.h" // USelection
#endif
#include "WebSocketsModule.h"

namespace
{
constexpr float ReconnectDelay = 2.0f;     // seconds between connection attempts
constexpr float TransientInterval = 0.033f; // ~30 Hz live streaming
constexpr int32 StillFramesForFinal = 10;   // rest frames before the final commit

/**
 * Request timeouts, in three tiers, and the tier is chosen by who is waiting.
 *
 * The numbers exist because of a real failure over a Cloudflare tunnel: `curl` fetched
 * /health in 0.12-0.27 s while the plugin's own request timed out at 5 s. Nothing was
 * wrong with the backend. UE's HTTP layer caps concurrent connections per server at 16
 * (`HttpMaxConnectionsPerServer`, HttpModule.cpp), the GLB downloads and the REST API
 * live on the same host and therefore share that pool, and the log showed exactly 16
 * meshes building. /health had queued behind the GLB fleet. A latency budget on this
 * host is not a measure of the backend's speed; it is a measure of how busy we are.
 *
 * So: background work that nobody is waiting on gets a budget long enough to outlast a
 * saturated pool, and interactive work keeps a short one because a human would rather
 * hear "no" quickly.
 */
constexpr float HealthDiagnosticTimeout = 5.0f;  // `Looma.Status` — someone is watching
constexpr float BackgroundRequestTimeout = 30.0f; // the quiet probe, /auth/me, the logout revoke
constexpr float InteractiveRequestTimeout = 15.0f; // a login: a person typed it and is waiting

/**
 * Quiet re-probe backoff. Unbounded in count, bounded in rate: a backend that is
 * genuinely down costs one cheap GET a minute, and the alternative — giving up — is a
 * client that never learns the answer for the rest of the session.
 */
constexpr float HealthRetryMinDelay = 5.0f;
constexpr float HealthRetryMaxDelay = 60.0f;

/**
 * The one spelling of the bearer header. Three places set it — the two ApplyAuthHeader
 * overloads and Logout, which cannot use them because it has deliberately already
 * forgotten the token — so the *format* is pulled out here rather than typed three
 * times. `parse_bearer_token` (backend/app/auth/provider.py) lowercases the scheme but
 * splits on the space, so "Bearer" without it is a token the backend never sees.
 */
constexpr const TCHAR* AuthHeaderName = TEXT("Authorization");
constexpr const TCHAR* AuthHeaderBearerPrefix = TEXT("Bearer ");
/** The guest-name seed header (HAM-176). See ULoomaSceneSyncSubsystem::ApplyClientIdHeader. */
constexpr const TCHAR* ClientIdHeaderName = TEXT("X-Client-Id");

/**
 * The handshake refusals that mean START OVER rather than TRY AGAIN
 * (docs/scene-format.md, "Performance selection (HAM-197)"). Every other close — a
 * dropped tunnel, a restarted uvicorn, a laptop lid — is worth retrying, and the
 * default therefore stays "retry"; these three are the enumerated exceptions.
 *
 * They arrive as BARE closes with no error frame, which is the contract's choice and
 * not an omission: a client that cannot parse an error body it was not expecting is no
 * better off than one that sees its socket close and knows to stop.
 */
constexpr int32 CloseMalformedHello = 4400;   // `expected hello with clientId`
constexpr int32 CloseKicked = 4403;           // this subject is kicked from that performance
constexpr int32 ClosePerformanceNotFound = 4404; // no such performance, or private and not ours

bool IsTerminalCloseCode(int32 Code)
{
    return Code == CloseMalformedHello || Code == CloseKicked || Code == ClosePerformanceNotFound;
}

/**
 * How many scenes a "nothing matched" message names before it gives up and counts.
 * A console line long enough to scroll its own explanation off the screen has stopped
 * being an explanation; the count keeps the message honest about what it left out.
 */
constexpr int32 MaxScenesToName = 20;

/**
 * Pad to a column width, for the listing commands. Ids run from `default` to
 * `demo-walk-in-kiosk-draft`, and a ragged left column is what turns a list somebody
 * asked for into one they have to read twice.
 */
FString Pad(const FString& Text, int32 Width)
{
    return Text.Len() >= Width ? Text : Text + FString::ChrN(Width - Text.Len(), TEXT(' '));
}

/** "id ('Name')" per scene, capped — for the messages that have to show the candidates. */
FString DescribeScenes(const TArray<FLoomaSceneSummary>& Scenes)
{
    if (Scenes.Num() == 0)
    {
        return TEXT("none");
    }
    const int32 Named = FMath::Min(Scenes.Num(), MaxScenesToName);
    TArray<FString> Parts;
    Parts.Reserve(Named);
    for (int32 Index = 0; Index < Named; ++Index)
    {
        Parts.Add(FString::Printf(TEXT("%s ('%s')"), *Scenes[Index].Id,
            Scenes[Index].Name.IsEmpty() ? TEXT("<no name>") : *Scenes[Index].Name));
    }
    FString Out = FString::Join(Parts, TEXT(", "));
    if (Scenes.Num() > Named)
    {
        Out += FString::Printf(TEXT(", and %d more"), Scenes.Num() - Named);
    }
    return Out;
}

/**
 * The persisted session's location and shape. `Saved/` because it is per-machine
 * scratch that no project commits — see GetSessionFilePath for why Project Settings
 * would be the wrong home.
 *
 * There is no format version. The loader requires every field it reads and discards
 * the file otherwise, so a future shape change is self-cleaning: an old file simply
 * fails to satisfy the new loader and is treated as no session at all. A half-written
 * file (a crash mid-save) lands in the same place, unparseable and therefore ignored.
 */
constexpr const TCHAR* SessionFileDir = TEXT("LoomaSceneSync");
constexpr const TCHAR* SessionFileName = TEXT("Session.json");
constexpr const TCHAR* SessionFieldBackend = TEXT("backend");
constexpr const TCHAR* SessionFieldToken = TEXT("token");
constexpr const TCHAR* SessionFieldDisplayName = TEXT("displayName");

/**
 * The settings' backend address as an absolute http(s) base with no trailing slash.
 * Accepts a bare "host:port" (the plugin's original format), a full URL with a path
 * prefix (a reverse proxy or tunnel), or a ws(s) URL — which is the same base with
 * the other scheme.
 */
FString NormalizeRestBase(const FString& Configured)
{
    FString Base = Configured.TrimStartAndEnd();
    if (Base.IsEmpty())
    {
        return TEXT("http://127.0.0.1:8000");
    }
    if (Base.StartsWith(TEXT("wss://")))
    {
        Base = TEXT("https://") + Base.RightChop(6);
    }
    else if (Base.StartsWith(TEXT("ws://")))
    {
        Base = TEXT("http://") + Base.RightChop(5);
    }
    else if (!Base.StartsWith(TEXT("http://")) && !Base.StartsWith(TEXT("https://")))
    {
        Base = TEXT("http://") + Base;
    }
    while (Base.EndsWith(TEXT("/")))
    {
        Base.LeftChopInline(1);
    }
    return Base;
}

/** The auth state as one word for a log line / the status string. */
const TCHAR* AuthStateText(ELoomaAuthState State)
{
    switch (State)
    {
    case ELoomaAuthState::Disabled:
        return TEXT("disabled");
    case ELoomaAuthState::EnabledRegistrationClosed:
        return TEXT("required (registration closed)");
    case ELoomaAuthState::EnabledRegistrationOpen:
        return TEXT("required (registration open)");
    default:
        return TEXT("unknown");
    }
}

/**
 * The `detail` string FastAPI puts on an HTTPException body, or empty.
 *
 * One named field, never an excerpt of the body: echoing a response into the log is
 * how a token ends up in a shared file, and while no error body from /auth/* carries
 * one today, "today" is not a property logging code should rest on. The backend's
 * own message is preferred over anything we could compose because it is the wording
 * the operator sees in every other client.
 */
FString ErrorDetail(const FHttpResponsePtr& Resp)
{
    if (!Resp.IsValid())
    {
        return FString();
    }
    TSharedPtr<FJsonObject> Json;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Resp->GetContentAsString());
    FString Detail;
    if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
    {
        Json->TryGetStringField(TEXT("detail"), Detail);
    }
    return Detail;
}

/**
 * The `auth` discovery block of a /health body, read as a state.
 *
 * Anything missing or of the wrong shape reads as Unknown — including an absent `auth`
 * altogether, which is what a backend older than HAM-172 answers. Such a backend does
 * in fact have no auth, so mapping it to Disabled would be right *today*; that is
 * precisely what makes it the wrong thing to write down. "The server said false" and
 * "the server said nothing" are different claims, and only the first one is evidence.
 * A caller that wants to treat silence as permission can still do so, deliberately,
 * from Unknown.
 */
ELoomaAuthState ReadAuthState(const TSharedPtr<FJsonObject>& Health)
{
    const TSharedPtr<FJsonObject>* Auth = nullptr;
    if (!Health.IsValid() || !Health->TryGetObjectField(TEXT("auth"), Auth) || !Auth || !Auth->IsValid())
    {
        return ELoomaAuthState::Unknown;
    }
    bool bEnabled = false;
    if (!(*Auth)->TryGetBoolField(TEXT("enabled"), bEnabled))
    {
        return ELoomaAuthState::Unknown;
    }
    if (!bEnabled)
    {
        return ELoomaAuthState::Disabled;
    }
    // camelCase on the wire, as everywhere else the backend speaks. Absent reads as
    // closed, which is the safe half of that guess: it withholds a "create account"
    // button rather than offering one the server will refuse, and unlike `enabled` it
    // cannot make a login screen disappear.
    bool bRegistrationEnabled = false;
    (*Auth)->TryGetBoolField(TEXT("registrationEnabled"), bRegistrationEnabled);
    return bRegistrationEnabled ? ELoomaAuthState::EnabledRegistrationOpen
                                : ELoomaAuthState::EnabledRegistrationClosed;
}

/**
 * The node an attachment names on the wire — pass GetAttachParentActor() — or null if
 * it names none, which is what a root is.
 *
 * Only another node can be a parent: the wire addresses parents by node id, and an
 * attachment to anything else has no id to name. That covers a plain level actor and
 * also an ALoomaSyncedActor someone dropped into the map by hand, which never went
 * through the hub and so has no `Id` — as unnameable as an AStaticMeshActor.
 */
ALoomaSyncedActor* WireParentNode(AActor* AttachedTo)
{
    ALoomaSyncedActor* Parent = Cast<ALoomaSyncedActor>(AttachedTo);
    return (Parent && !Parent->Id.IsEmpty()) ? Parent : nullptr;
}

/**
 * The pose to put on the wire for this actor: parent-local under a node, its **world**
 * pose otherwise.
 *
 * For a root the two are the same value — an unattached root component's relative
 * transform *is* its world transform — so this only bites in one case: an actor
 * attached to something the wire cannot name. There we report the node as a root
 * (see TickOutbound), and a root's `t` is a world pose, so it must be read as one.
 * Anything else would send the browser a pose measured in a frame it has never heard
 * of. A node parent stays strictly local, which is the invariant that keeps a child
 * from re-broadcasting its parent's motion as its own.
 */
FTransform WirePose(const ALoomaSyncedActor& Actor)
{
    return WireParentNode(Actor.GetAttachParentActor()) ? Actor.GetLocalTransform() : Actor.GetActorTransform();
}
} // namespace

// --- Lifecycle ---------------------------------------------------------------

void ULoomaSceneSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
    FModuleManager::Get().LoadModuleChecked(TEXT("WebSockets"));
    SettingsChangedHandle = ULoomaSceneSyncSettings::OnSettingsChanged().AddUObject(
        this, &ULoomaSceneSyncSubsystem::OnSettingsChanged);
    // Before Connect, and that order is the whole point of it: the first handshake then
    // already carries the bearer, so the hub names us the account from the outset. Load
    // it afterwards instead and every launch would connect as a guest and immediately
    // reconnect, which every other client in the room sees as a join/leave flicker.
    //
    // Validation is not done here. The health probe below is the thing that already
    // knows when the backend became reachable, and retries until it does, so the
    // restored token is checked from its success path — see ProbeHealth.
    LoadSavedSession();
    Connect();
    // Ask what this backend wants before anything asks us. A UI that decides whether
    // to show a login screen cannot wait for a human to type `Looma.Status`, and the
    // socket is no substitute for the answer: an auth-enabled hub may well refuse the
    // WebSocket, which is a symptom of the answer rather than a way to get it.
    ProbeHealth(/*bLogDiagnostics=*/false);

#if WITH_EDITOR
    // Mirror the editor's own selection while PIE runs, so clicking a node in the
    // Outliner puts a border on it in every other client. Static event, so this hears
    // about every USelection in the editor and not only ours — the recompute is cheap
    // and the diff throws away the ones that changed nothing.
    EditorSelectionChangedHandle = USelection::SelectionChangedEvent.AddUObject(
        this, &ULoomaSceneSyncSubsystem::OnEditorSelectionChanged);
#endif
}

void ULoomaSceneSyncSubsystem::Deinitialize()
{
#if WITH_EDITOR
    // Explicitly, even though AddUObject already holds us weakly and would not fire
    // into a destroyed subsystem: SelectionChangedEvent is a *static* event that
    // outlives every game instance, so a binding left on it is a leak that accumulates
    // one entry per PIE session for the life of the editor process.
    USelection::SelectionChangedEvent.Remove(EditorSelectionChangedHandle);
    EditorSelectionChangedHandle.Reset();
#endif
    ULoomaSceneSyncSettings::OnSettingsChanged().Remove(SettingsChangedHandle);
    SettingsChangedHandle.Reset();
    CloseSocket();
    // Drop the in-memory copy only. The saved session on disk is deliberately left
    // alone: outliving the game instance is the entire point of it, and closing the
    // editor is not a logout — ClearSession is the one thing that deletes the file.
    //
    // Tidiness rather than a security control, and worth being exact about, since a
    // token now does rest on disk: Empty() releases the buffer without scrubbing it,
    // and scrubbing this one would buy nothing while the HTTP layer and every
    // `Bearer …` header string hold copies we neither own nor can reach.
    AuthToken.Empty();
    CurrentIdentity = FLoomaIdentity();
    Tracked.Empty();
    JobHandles.Empty();
    PendingHandleReplays.Empty();
    SuggestedTransforms.Empty();
    ActiveSceneId.Reset();
    ActivePerformance = FLoomaPerformance();
    RequestedPerformanceId.Reset();
    SentPerformanceId.Reset();
    PendingCueIndex = INDEX_NONE;
    Super::Deinitialize();
}

void ULoomaSceneSyncSubsystem::CloseSocket()
{
    if (!Socket.IsValid())
    {
        return;
    }
    // Clear first: a socket we are abandoning must not schedule a retry from its own
    // OnClosed, which would race the connection we are about to make.
    Socket->OnConnected().Clear();
    Socket->OnConnectionError().Clear();
    Socket->OnClosed().Clear();
    Socket->OnMessage().Clear();
    Socket->Close();
    Socket.Reset();
    bConnecting = false;
    // The room was derived from this socket's registration in the hub's presence
    // table, so it means nothing the moment the socket goes. Note the scene state is
    // deliberately NOT cleared alongside it — the hub answers a fresh connection with
    // the whole `scene`, which reconciles it, whereas nothing ever replays a roster we
    // kept a stale copy of.
    ClearPresence();
}

void ULoomaSceneSyncSubsystem::Connect()
{
    CloseSocket();
    SocketUrl = GetSceneSyncUrl();
    // Read here, where the socket is made, rather than inside OnConnected — so that
    // what we recorded and what we sent cannot drift if the settings change between
    // creating the socket and it opening.
    SentDisplayName = ULoomaSceneSyncSettings::Get().GuestDisplayName.TrimStartAndEnd();
    // Snapshotted here for the same reason as the line above and with a sharper edge:
    // this one is compared against the hub's answer once the `scene` frame lands, so a
    // request that moved in between — a second `Looma.Performance` typed while the first
    // switch is still coming up — would make us announce a mismatch that never happened.
    // RequestedPerformanceId is the durable intent; this is what this socket said.
    SentPerformanceId = RequestedPerformanceId;
    bAwaitingPerformanceConfirmation = !SentPerformanceId.IsEmpty();
    // Making a socket supersedes the refusal that stopped the last one. Cleared here
    // rather than in Reconnect() so that every door in — a console reconnect, a backend
    // address change, Initialize — goes through one line, and none of them has to
    // remember. Nothing reaches here on its own while the code is set: a terminal close
    // arms no cooldown, so Tick never calls Connect().
    TerminalCloseCode = 0;
    // The handshake carries the session, so the hub resolves this socket as the account
    // rather than as a guest — the roster every other client draws comes from what it
    // decides here, once, at connect time. Which is why a login has to reconnect: there
    // is no message that re-identifies a socket already up.
    TMap<FString, FString> UpgradeHeaders;
    ApplyAuthHeader(UpgradeHeaders);
    Socket = FWebSocketsModule::Get().CreateWebSocket(SocketUrl, TEXT(""), UpgradeHeaders);
    bConnecting = true;

    Socket->OnConnected().AddWeakLambda(this, [this]() {
        bConnecting = false;
        UE_LOG(LogLoomaSync, Log, TEXT("Connected to %s"), *SocketUrl);
        TSharedRef<FJsonObject> Hello = MakeShared<FJsonObject>();
        Hello->SetStringField(TEXT("type"), TEXT("hello"));
        Hello->SetStringField(TEXT("clientId"), ClientId);
        Hello->SetStringField(TEXT("role"), TEXT("unreal"));
        // Suggest a roster name. With none, the hub names us `Guest-` plus the first
        // six characters of our clientId — an unreadable string in everybody else's
        // roster. `GET /auth/me` mints a fresh random guest name on every call and so
        // cannot report it; since HAM-159 the inbound `clients` roster can (see
        // GetOwnClientId / GetClient), but proposing a name here is still the only way
        // to have a readable one to read back.
        //
        // Decides anything only for a GUEST connection: identity_from_websocket
        // resolves a token first and never consults the suggestion when one resolves
        // (backend/app/auth/local.py), so this renames a guest and never an account.
        // Sent unconditionally all the same, because branching on the token to omit a
        // field the hub already ignores would be more code for the same outcome.
        //
        // Sent raw. The hub clamps it, strips it, and may reject it outright; it is the
        // authority on what a roster name may be, so a second opinion here could only
        // disagree with it. The empty check is not validation — it is declining to emit
        // a field whose only possible outcome is rejection.
        if (!SentDisplayName.IsEmpty())
        {
            Hello->SetStringField(TEXT("displayName"), SentDisplayName);
        }
        // Which room to join, and OMITTED ENTIRELY when nothing was asked for — never
        // sent empty. An absent field is what lands a client wherever the hub decides:
        // the room this `clientKey` was last in, else `default`. That is what every
        // build before this one did and it must keep working exactly so. An empty
        // string is not the same request: `sync.py` treats only a non-empty string as
        // an explicit choice, and a client that sent "" would be asking for a
        // performance whose id is "" and collecting a 4404 for it.
        //
        // A request and not a claim, like `displayName` above: the hub re-resolves
        // identity and re-runs the visibility gate, so this decides nothing on its own.
        if (!SentPerformanceId.IsEmpty())
        {
            Hello->SetStringField(TEXT("performanceId"), SentPerformanceId);
        }
        // Belt and braces, both documented: the hub's precedence is bearer header,
        // then session cookie, then `hello.token` (docs/scene-format.md), and all three
        // resolve to the same identity. The header wins when it survives the trip; the
        // hello field is what covers a proxy that strips Authorization on upgrade,
        // which this deployment — behind a Cloudflare tunnel — is exactly the shape of.
        // Sending both costs one field and removes a class of "works locally" bug.
        ApplyAuthToken(Hello);
        SendJson(Hello);
        OnSyncConnected.Broadcast();
        // A reconnecting client "comes back with an empty selection" — the socket that
        // made the old claim is gone and nothing would ever retract it — so the
        // send-on-connect the contract requires is what brings our borders back
        // (docs/scene-format.md, "What each client has selected"). Flagged rather than
        // sent from here so it goes out through the one diffing send path, and so it
        // cannot race the `hello` this socket has only just written.
        bForceSelectionSend = true;
        // The WS never replays generation events to late joiners — pull the
        // current queue over REST so cached jobs reflect all clients.
        HydrateGenerationQueue();
        // No auth probe here, though this used to be the obvious place for one — it
        // covered the editor that opened before uvicorn did. Connect time turns out to
        // be the *worst* moment to ask: this is the instant the scene arrives and every
        // node starts pulling its GLB, so a request issued here queues behind up to 16
        // downloads on the shared connection pool and times out against a backend that
        // is answering in a tenth of a second (see the timeout constants at the top of
        // this file). The retry backoff covers the launched-too-early case without
        // competing for a slot at the one moment they are all taken.
    });
    Socket->OnConnectionError().AddWeakLambda(this, [this](const FString& Error) {
        bConnecting = false;
        UE_LOG(LogLoomaSync, Warning, TEXT("Connection error on %s: %s (retrying)"), *SocketUrl, *Error);
        ReconnectCooldown = ReconnectDelay;
        // Before the disconnect event, not after: a listener that reads GetClients()
        // from OnSyncDisconnected must see the empty room, not the one that just died.
        ClearPresence();
        OnSyncDisconnected.Broadcast();
    });
    Socket->OnClosed().AddWeakLambda(this, [this](int32 Code, const FString& Reason, bool bWasClean) {
        bConnecting = false;
        // Retry unless the hub has told us the handshake itself is the problem. Only
        // this handler can make that call: OnConnectionError above never sees a code —
        // a TCP-level failure has none — so it stays an unconditional retry, which is
        // right for it. The two paths agree on everything else, including broadcasting
        // last.
        if (IsTerminalCloseCode(Code))
        {
            HandleTerminalClose(Code, Reason);
        }
        else
        {
            UE_LOG(LogLoomaSync, Warning, TEXT("Socket closed (%d %s), retrying"), Code, *Reason);
            ReconnectCooldown = ReconnectDelay;
        }
        // Dropped here as well as in CloseSocket, and on both branches above: neither
        // this handler nor OnConnectionError routes through CloseSocket, so without
        // this a departed room would outlive the socket that described it for as long
        // as the reconnect took. Before the disconnect event, not after, so a listener
        // reading GetClients() from OnSyncDisconnected sees the empty room.
        ClearPresence();
        OnSyncDisconnected.Broadcast();
    });
    Socket->OnMessage().AddUObject(this, &ULoomaSceneSyncSubsystem::OnRawMessage);
    Socket->Connect();
}

void ULoomaSceneSyncSubsystem::SendJson(const TSharedRef<FJsonObject>& Msg)
{
    if (!Socket.IsValid() || !Socket->IsConnected())
    {
        return;
    }
    Msg->SetStringField(TEXT("origin"), ClientId);
    // Nothing here logs Text, and nothing may start to: since the `hello` carries the
    // session token, a "here is what we sent" line would put a credential in every
    // log file this plugin ever produces. Log the message *type* if you need a trace.
    FString Text;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Msg, Writer);
    Socket->Send(Text);
}

bool ULoomaSceneSyncSubsystem::IsSyncConnected() const
{
    return Socket.IsValid() && Socket->IsConnected();
}

// --- Which scene this client is on -------------------------------------------

void ULoomaSceneSyncSubsystem::OpenScene(const FString& SceneId)
{
    if (!IsSyncConnected())
    {
        // SendJson would drop this without a word. That silence is right for a pose
        // diff the next tick sends again and wrong for a one-shot intent that nothing
        // repeats, so the exception is made here rather than in SendJson — this is the
        // sender that cannot survive being ignored, not a reason to make every send
        // noisy.
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Cannot open scene '%s': the scene-sync socket is not connected, and a send on a ")
            TEXT("closed socket is dropped rather than queued. `Looma.Status` says why, ")
            TEXT("`Looma.Reconnect` retries now."),
            *SceneId);
        return;
    }
    TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
    Msg->SetStringField(TEXT("type"), TEXT("openScene"));
    Msg->SetStringField(TEXT("sceneId"), SceneId);
    SendJson(Msg);
    // Sent, which is all this can honestly claim. What actually happened arrives later
    // as a `scene` frame or as a `sceneError`, both of which log for themselves.
    UE_LOG(LogLoomaSync, Log, TEXT("openScene '%s' sent"), *SceneId);
}

FString ULoomaSceneSyncSubsystem::GetActiveSceneId() const
{
    return ActiveSceneId;
}

void ULoomaSceneSyncSubsystem::LogActiveScene()
{
    if (ActiveSceneId.IsEmpty())
    {
        // Two different emptinesses, and they must not read alike. The hub sends
        // `sceneId: null` for a working scene nobody has saved — a real scene, with a
        // real document in it, that merely has no row to be named by. Before the first
        // `scene` frame lands we hold the same empty string and know nothing at all.
        // Only the socket separates them, which is why GetActiveSceneId cannot.
        if (IsSyncConnected())
        {
            UE_LOG(LogLoomaSync, Display,
                TEXT("Active scene: <unsaved> — the hub has us on a working scene with no saved row ")
                TEXT("behind it, so it has neither an id nor a name."));
        }
        else
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Active scene: unknown — the socket is not connected, so no `scene` frame has ")
                TEXT("named one. `Looma.Status` says why."));
        }
        return;
    }
    // The id first and on its own, because it is the half we already know and it must
    // not be held hostage to a round trip that can fail. The name follows below.
    UE_LOG(LogLoomaSync, Display, TEXT("Active scene id '%s' (asking the backend for its name)"),
        *ActiveSceneId);

    // Captured, not re-read on arrival: this answer describes the scene we asked about,
    // and `Looma.Scene <other>` may have been typed in the meantime.
    const FString SceneId = ActiveSceneId;
    FetchScenes([SceneId](bool bOk, const TArray<FLoomaSceneSummary>& Scenes) {
        if (!bOk)
        {
            // FetchScenes has already said what failed and where. This adds what it
            // cost, which is the half a reader of this command needs: the id printed
            // above is still true, and only the name is missing.
            UE_LOG(LogLoomaSync, Warning,
                TEXT("...so the name of scene '%s' cannot be read. The id above still stands."),
                *SceneId);
            return;
        }
        const FLoomaSceneSummary* Row = Scenes.FindByPredicate(
            [&SceneId](const FLoomaSceneSummary& Candidate) { return Candidate.Id == SceneId; });
        if (!Row)
        {
            // Neither a failure nor a missing name: the list is non-enumerating, so a
            // scene absent from it is one this IDENTITY may not read. Reachable in
            // ordinary use rather than theoretical — the socket resolved us once, at
            // handshake time, and this HTTP call resolves us again, so a guest whose
            // `X-Client-Id` anchor does not match what the hub recorded lands exactly
            // here. Said out loud, because "no name" would read as a backend fault.
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Scene '%s' is not in the list this identity is offered, so its name cannot be ")
                TEXT("read — we are still on it, but this request resolved as a different caller ")
                TEXT("than the socket did."),
                *SceneId);
            return;
        }
        UE_LOG(LogLoomaSync, Display, TEXT("Active scene '%s' is named '%s'"),
            *SceneId, Row->Name.IsEmpty() ? TEXT("<no name>") : *Row->Name);
    });
}

void ULoomaSceneSyncSubsystem::LogScenes()
{
    // Read before the fetch goes out, for the same reason LogActiveScene captures it:
    // the answer describes the catalogue as it was asked for, and a `scene` frame may
    // land while it is in flight.
    const FString Active = ActiveSceneId;
    FetchScenes([Active](bool bOk, const TArray<FLoomaSceneSummary>& Scenes) {
        if (!bOk)
        {
            // FetchScenes has already said what failed and where; this says what it
            // cost, which for this command is everything it was asked to do.
            UE_LOG(LogLoomaSync, Warning, TEXT("...so there is no scene list to show."));
            return;
        }
        if (Scenes.Num() == 0)
        {
            // Not an empty printout with a zero on it: the list is filtered per
            // identity, so "none" is a fact about who the backend thinks we are, and
            // that is the thing worth pointing at.
            UE_LOG(LogLoomaSync, Display,
                TEXT("No scenes are offered to this identity. `Looma.Whoami` says who the backend ")
                TEXT("thinks we are."));
            return;
        }
        int32 Width = 0;
        for (const FLoomaSceneSummary& Row : Scenes)
        {
            Width = FMath::Max(Width, Row.Id.Len());
        }
        UE_LOG(LogLoomaSync, Display, TEXT("%d scene(s) this identity may open:"), Scenes.Num());
        bool bMarkedActive = false;
        for (const FLoomaSceneSummary& Row : Scenes)
        {
            const bool bIsActive = !Active.IsEmpty() && Row.Id == Active;
            bMarkedActive = bMarkedActive || bIsActive;
            UE_LOG(LogLoomaSync, Display, TEXT("%s %s  '%s'%s"),
                bIsActive ? TEXT("*") : TEXT(" "),
                *Pad(Row.Id, Width),
                Row.Name.IsEmpty() ? TEXT("<no name>") : *Row.Name,
                bIsActive ? TEXT("   (active)") : TEXT(""));
        }
        if (Active.IsEmpty())
        {
            // Nothing marked is a state, not an omission, and the two states behind it
            // are not this command's to explain — `Looma.Scene` already separates "the
            // working scene has never been saved" from "no frame has arrived", and
            // saying it twice is how the two copies come to disagree.
            UE_LOG(LogLoomaSync, Display,
                TEXT("None is marked active: no saved scene is open. `Looma.Scene` with no ")
                TEXT("arguments says which of the two reasons applies."));
        }
        else if (!bMarkedActive)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("The active scene '%s' is not in this list, so the socket resolved us as one ")
                TEXT("identity and this request as another — the subscription is real, the list is ")
                TEXT("simply filtered for somebody else."),
                *Active);
        }
    });
}

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> ULoomaSceneSyncSubsystem::MakeCatalogueRequest(
    const FString& Url) const
{
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(Url);
    Request->SetVerb(TEXT("GET"));
    // The interactive budget: a person typed a command and is watching the log, and
    // would rather hear "no" quickly than "yes" eventually.
    Request->SetTimeout(InteractiveRequestTimeout);
    ApplyAuthHeader(Request); // after SetURL, as it requires
    // Wider than ApplyClientIdHeader's stated "asset-creating requests" scope, and
    // deliberately so: `X-Client-Id` is what anchors a GUEST to one subject across
    // calls, so without it a guest's own private rows are missing from the list a
    // catalogue route returns — which makes them unopenable by name and unfindable in a
    // listing, not merely unnamed. The contract asks for it here by name — "Send the
    // header — the web client does, on every `/scenes` call" (docs/scene-format.md), and
    // `/performances` resolves a guest through the identical `subject_key`. It still
    // proves nothing: a resolved session always wins over it.
    //
    // Both headers fail SILENTLY when they are wrong — a missing bearer or a missing
    // anchor returns a shorter list, not an error — which is the whole reason this is
    // one function rather than a shape each route repeats.
    ApplyClientIdHeader(Request);
    return Request;
}

void ULoomaSceneSyncSubsystem::FetchScenes(
    TFunction<void(bool bOk, const TArray<FLoomaSceneSummary>& Scenes)> OnDone)
{
    // GET /scenes, not GET /scenes/{id}. The single-scene route asks the narrower
    // question and would answer a clean 404 for an id we may not have — but it inlines
    // the whole node document, which is the one thing this client already holds and the
    // largest payload the backend serves. Re-fetching a scene graph across the same
    // 16-slot connection pool the GLB downloads live in, to read one string off it, is
    // the wrong trade for a console command. The list route is metadata only, is
    // filtered to what this caller may see through the same `effective_role` gate the
    // single-scene route uses (backend/app/scenes.py), and is what the web scene panel
    // already leans on, so it is the well-trodden path as well as the small one.
    //
    // It is also the only route that can answer "which scenes are there at all", which
    // is what makes one fetch serve a name lookup, an id lookup and a listing.
    const FString ListUrl = GetRestBase() + TEXT("/scenes");
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeCatalogueRequest(ListUrl);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [ListUrl, OnDone](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            // Named here, once, rather than in each caller: the URL and the status code
            // are facts about the request, and only the *consequence* differs per
            // caller. Both halves get said — this line is what failed, the caller's is
            // what it cost.
            TArray<FLoomaSceneSummary> Scenes;
            if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 300)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("Could not read the scene list from %s (%d)"),
                    *ListUrl, Resp.IsValid() ? Resp->GetResponseCode() : 0);
                OnDone(false, Scenes);
                return;
            }
            TArray<TSharedPtr<FJsonValue>> Rows;
            const TSharedRef<TJsonReader<TCHAR>> Reader =
                TJsonReaderFactory<TCHAR>::Create(Resp->GetContentAsString());
            if (!FJsonSerializer::Deserialize(Reader, Rows))
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("%s answered 200 but not with a scene list"), *ListUrl);
                OnDone(false, Scenes);
                return;
            }
            Scenes.Reserve(Rows.Num());
            for (const TSharedPtr<FJsonValue>& Row : Rows)
            {
                const TSharedPtr<FJsonObject> Entry = Row.IsValid() ? Row->AsObject() : nullptr;
                FLoomaSceneSummary Summary;
                // A row with no id is not a scene anything can act on — it cannot be
                // opened and cannot be pointed at — so it is dropped rather than
                // carried as a half-row every caller would have to check for. Dropping
                // it cannot make a name lookup lie either: the row it removes is one
                // that could never have been opened even if its name had matched, so
                // "no scene has that name" stays the true answer rather than becoming a
                // convenient one. The name is taken as it comes — an empty one is a
                // display problem, not a reason to discard the row.
                if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("id"), Summary.Id)
                    || Summary.Id.IsEmpty())
                {
                    continue;
                }
                Entry->TryGetStringField(TEXT("name"), Summary.Name);
                Scenes.Add(MoveTemp(Summary));
            }
            OnDone(true, Scenes);
        });
    Request->ProcessRequest();
}

void ULoomaSceneSyncSubsystem::OpenSceneByNameOrId(const FString& NameOrId)
{
    const FString Wanted = NameOrId.TrimStartAndEnd();
    if (Wanted.IsEmpty())
    {
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Opening a scene needs an id or a name — nothing was sent."));
        return;
    }
    FetchScenes([this, Wanted](bool bOk, const TArray<FLoomaSceneSummary>& Scenes) {
        if (!bOk)
        {
            // Degrade to exactly what step 1 did: send it as an id and let the hub be
            // the authority, since it is the only one left standing. Not a guess and
            // not a silent retry — the log says which reading was chosen and what the
            // other reading would now look like.
            UE_LOG(LogLoomaSync, Warning,
                TEXT("...so '%s' cannot be looked up, and is being sent as an id for the hub to ")
                TEXT("answer. If it was a name, that answer will be a `sceneError`."),
                *Wanted);
            OpenScene(Wanted);
            return;
        }
        // 1. An exact id. First because an id names exactly one scene and is what every
        //    other line in this plugin prints, so a string that IS an id must always
        //    mean that scene.
        const FLoomaSceneSummary* ById = Scenes.FindByPredicate(
            [&Wanted](const FLoomaSceneSummary& Candidate) { return Candidate.Id == Wanted; });
        if (ById)
        {
            // A collision between KINDS, not within names: this string is one scene's
            // id and another's name. Resolving it silently is what would make the next
            // person's bug report unreadable, so the losing reading is named along with
            // the id that reaches it.
            TArray<FString> AlsoNamed;
            for (const FLoomaSceneSummary& Candidate : Scenes)
            {
                if (Candidate.Id != ById->Id && Candidate.Name.Equals(Wanted, ESearchCase::IgnoreCase))
                {
                    AlsoNamed.Add(Candidate.Id);
                }
            }
            if (AlsoNamed.Num() > 0)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("'%s' is the ID of one scene and the NAME of %s. Opening it as the id, ")
                    TEXT("because an id names exactly one scene; open the other by its id."),
                    *Wanted, *FString::Join(AlsoNamed, TEXT(", ")));
            }
            OpenScene(ById->Id);
            return;
        }
        // 2. An exact name, then 3. a name ignoring case. Two passes rather than one
        //    case-insensitive one, so that where both exist the exactly-typed name wins
        //    outright instead of being reported as an ambiguity the user did not create.
        TArray<const FLoomaSceneSummary*> Exact;
        TArray<const FLoomaSceneSummary*> Insensitive;
        for (const FLoomaSceneSummary& Candidate : Scenes)
        {
            if (Candidate.Name.Equals(Wanted, ESearchCase::CaseSensitive))
            {
                Exact.Add(&Candidate);
            }
            else if (Candidate.Name.Equals(Wanted, ESearchCase::IgnoreCase))
            {
                Insensitive.Add(&Candidate);
            }
        }
        const bool bMatchedExactly = Exact.Num() > 0;
        const TArray<const FLoomaSceneSummary*>& Matches = bMatchedExactly ? Exact : Insensitive;
        if (Matches.Num() == 1)
        {
            const FLoomaSceneSummary& Match = *Matches[0];
            // Said out loud, because the user typed one string and is about to be moved
            // to a scene identified by another: the id is what every later line will
            // call it, so this is where the two are connected.
            UE_LOG(LogLoomaSync, Display, TEXT("Scene '%s' is '%s'%s; opening it."),
                *Match.Name, *Match.Id,
                bMatchedExactly ? TEXT("") : TEXT(" — matched ignoring case"));
            OpenScene(Match.Id);
            return;
        }
        if (Matches.Num() > 1)
        {
            // Refused, not resolved to the first. Nothing stops two scenes sharing a
            // name — `_slugify_scene` uniquifies the ID with -2, -3 precisely so the
            // name does not have to be unique (backend/app/scenes.py) — so "the first
            // one" is an ordering artefact of `order_idx`, and opening by it would be
            // choosing for the user invisibly. The ids are what tell them apart, so the
            // ids are what the refusal carries.
            TArray<FString> Ids;
            Ids.Reserve(Matches.Num());
            for (const FLoomaSceneSummary* Match : Matches)
            {
                Ids.Add(Match->Id);
            }
            UE_LOG(LogLoomaSync, Warning,
                TEXT("%d scenes are named '%s'%s, so the name does not say which: %s. Names are not ")
                TEXT("unique here and ids are — open one by its id."),
                Matches.Num(), *Wanted,
                bMatchedExactly ? TEXT("") : TEXT(" ignoring case"),
                *FString::Join(Ids, TEXT(", ")));
            return;
        }
        if (Scenes.Num() == 0)
        {
            // Distinct from "no match among these", and a real state rather than a
            // defensive branch: `GET /scenes` is filtered per identity, so a guest whose
            // anchor the backend does not recognise is offered an empty list and every
            // name would "not match" for a reason that has nothing to do with the name.
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Cannot open '%s': this identity is offered no scenes at all. `Looma.Whoami` ")
                TEXT("says who the backend thinks we are."),
                *Wanted);
            return;
        }
        // The whole point of resolving client-side: the miss is answered with the
        // candidates in hand, where the hub's `sceneError` can only say no.
        UE_LOG(LogLoomaSync, Warning,
            TEXT("No scene has the id or the name '%s'. %d offered to this identity: %s. ")
            TEXT("`Looma.Scenes` lists them all, one per line and untruncated. A name with spaces ")
            TEXT("can be quoted — Looma.Scene \"Costume Test\" — though it does not have to be."),
            *Wanted, Scenes.Num(), *DescribeScenes(Scenes));
    });
}

// --- Which performance this client is in -------------------------------------

FLoomaPerformance ULoomaSceneSyncSubsystem::GetActivePerformance() const
{
    return ActivePerformance;
}

FString ULoomaSceneSyncSubsystem::GetActivePerformanceId() const
{
    return ActivePerformance.Id;
}

FString ULoomaSceneSyncSubsystem::GetPendingPerformanceId() const
{
    return bAwaitingPerformanceConfirmation ? SentPerformanceId : FString();
}

void ULoomaSceneSyncSubsystem::FetchPerformances(
    TFunction<void(bool bOk, const TArray<FLoomaPerformanceSummary>& Performances)> OnDone)
{
    const FString ListUrl = GetRestBase() + TEXT("/performances");
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeCatalogueRequest(ListUrl);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [ListUrl, OnDone](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            TArray<FLoomaPerformanceSummary> Performances;
            if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 300)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("Could not read the performance list from %s (%d)"),
                    *ListUrl, Resp.IsValid() ? Resp->GetResponseCode() : 0);
                OnDone(false, Performances);
                return;
            }
            // AN OBJECT WRAPPING AN ARRAY, unlike `/scenes`, which answers with a bare
            // array. Worth stating rather than inferring from the code below: the two
            // catalogue routes really do differ here, so a reader who assumes the
            // symmetry the rest of this pair has will write the wrong parse.
            TSharedPtr<FJsonObject> Envelope;
            const TSharedRef<TJsonReader<TCHAR>> Reader =
                TJsonReaderFactory<TCHAR>::Create(Resp->GetContentAsString());
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid()
                || !Envelope->TryGetArrayField(TEXT("performances"), Rows) || !Rows)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("%s answered 200 but not with a performance list"), *ListUrl);
                OnDone(false, Performances);
                return;
            }
            Performances.Reserve(Rows->Num());
            for (const TSharedPtr<FJsonValue>& Row : *Rows)
            {
                const TSharedPtr<FJsonObject> Entry = Row.IsValid() ? Row->AsObject() : nullptr;
                FLoomaPerformanceSummary Summary;
                // Same rule as the scene rows: no id, no row. An id is the only thing a
                // performance can be switched to by, so one without it is a listing
                // entry that could only ever be looked at.
                if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("id"), Summary.Id)
                    || Summary.Id.IsEmpty())
                {
                    continue;
                }
                // Both nullable, and left empty rather than defaulted. `owner_name` is
                // null for an unowned row and `name` can be too; inventing "Untitled"
                // here would put a word on screen the backend never said.
                Entry->TryGetStringField(TEXT("name"), Summary.Name);
                Entry->TryGetStringField(TEXT("role"), Summary.Role);
                Performances.Add(MoveTemp(Summary));
            }
            OnDone(true, Performances);
        });
    Request->ProcessRequest();
}

void ULoomaSceneSyncSubsystem::LogPerformances()
{
    // Both read before the fetch: they are what the marker means, and a `scene` frame
    // landing mid-flight would otherwise mark the list against a room we learned about
    // after asking.
    const FString Current = ActivePerformance.Id;
    const FString Pending = GetPendingPerformanceId();
    FetchPerformances(
        [Current, Pending](bool bOk, const TArray<FLoomaPerformanceSummary>& Performances) {
            if (!bOk)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("...so there is no performance list to show."));
                return;
            }
            if (Performances.Num() == 0)
            {
                UE_LOG(LogLoomaSync, Display,
                    TEXT("No performances are visible to this identity — not even `default`, which ")
                    TEXT("is public, so this is a backend that answered without its seed row."));
                return;
            }
            if (!Pending.IsEmpty())
            {
                // Said before the list, because it changes what every marker below
                // means. This is the window step 3 built GetPendingPerformanceId for:
                // nothing clears the confirmed id on a drop, so it names the room being
                // left and looks entirely valid while it does.
                UE_LOG(LogLoomaSync, Display,
                    TEXT("A switch to '%s' is in flight and unconfirmed. %s"),
                    *Pending,
                    Current.IsEmpty()
                        ? TEXT("No `scene` frame has named a room yet, so nothing below is current.")
                        : TEXT("The row marked below is the one being LEFT, not the one we land in."));
            }
            int32 Width = 0;
            for (const FLoomaPerformanceSummary& Row : Performances)
            {
                Width = FMath::Max(Width, Row.Id.Len());
            }
            UE_LOG(LogLoomaSync, Display, TEXT("%d performance(s) visible to this identity:"),
                Performances.Num());
            bool bFoundCurrent = false;
            bool bFoundPending = false;
            for (const FLoomaPerformanceSummary& Row : Performances)
            {
                const bool bIsPending = !Pending.IsEmpty() && Row.Id == Pending;
                const bool bIsCurrent = !Current.IsEmpty() && Row.Id == Current;
                bFoundPending = bFoundPending || bIsPending;
                bFoundCurrent = bFoundCurrent || bIsCurrent;
                // Pending wins the label where both are the same row — asking again for
                // the room you are in is a real thing to type, and "requested" is the
                // newer and more surprising of the two facts.
                const TCHAR* Mark = TEXT("");
                if (bIsPending)
                {
                    Mark = TEXT("   (requested, not confirmed)");
                }
                else if (bIsCurrent)
                {
                    Mark = Pending.IsEmpty() ? TEXT("   (current)") : TEXT("   (leaving)");
                }
                UE_LOG(LogLoomaSync, Display, TEXT("%s %s  '%s'  [%s]%s"),
                    (bIsPending || bIsCurrent) ? TEXT("*") : TEXT(" "),
                    *Pad(Row.Id, Width),
                    Row.Name.IsEmpty() ? TEXT("<no name>") : *Row.Name,
                    Row.Role.IsEmpty() ? TEXT("no role") : *Row.Role,
                    Mark);
            }
            // A row missing from this list is one this identity cannot JOIN either, so
            // the listing is an honest preview of what a switch will accept. Checked
            // rather than assumed, because the two questions are asked by different
            // code: `list_visible` filters `state="active"` itself, while the
            // handshake's `get_visible` -> `can_read` -> `effective_role` returns None
            // for a non-active row as its FIRST statement — so archived, nonexistent and
            // not-yours all collapse into the same 4404 (backend/app/performances.py).
            //
            // The scenes side reaches the same agreement by a different route —
            // `list_readable_scenes` is four set lookups where `effective_role` is a
            // per-row call — which makes it the one that has to be verified rather than
            // inferred from this one. Neither pair may be assumed from the other.
            if (!Current.IsEmpty() && !bFoundCurrent)
            {
                // Two causes, and both are named because the remedies differ. A
                // performance archived while we sat in it keeps THIS socket working and
                // stops accepting new ones, so we could not rejoin the room we are in.
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("The current performance '%s' is not in this list: either it was archived ")
                    TEXT("while this socket was in it — still working, no longer joinable — or this ")
                    TEXT("request resolved as a different identity than the socket did."),
                    *Current);
            }
            if (!Pending.IsEmpty() && !bFoundPending)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("The requested performance '%s' is not one this identity may join, so the ")
                    TEXT("switch in flight will close with 4404 — unless access to it changed ")
                    TEXT("between these two requests."),
                    *Pending);
            }
        });
}

void ULoomaSceneSyncSubsystem::FetchCues(const FString& PerformanceId,
    TFunction<void(bool bOk, const TArray<FLoomaCue>& Cues)> OnDone)
{
    // Escaped, and the note in the header about where escaping would earn its place was
    // half wrong, so it is corrected here rather than left to look prophetic: the
    // prediction was that a command taking a performance id from a person would build
    // this path, and no command does. Both callers pass the CONFIRMED id — the fire
    // path deliberately reads the rail of where we landed, not where we asked — so a
    // typed string never reaches a URL. What does reach it is still a string from the
    // wire being spliced into a path, and one call is cheaper than an argument about
    // whether a `/` could ever appear in an id the hub minted.
    const FString ListUrl = GetRestBase()
        + FString::Printf(TEXT("/performances/%s/cues"),
            *FGenericPlatformHttp::UrlEncode(PerformanceId));
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeCatalogueRequest(ListUrl);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [ListUrl, OnDone](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            TArray<FLoomaCue> Cues;
            if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 300)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("Could not read the running order from %s (%d)"),
                    *ListUrl, Resp.IsValid() ? Resp->GetResponseCode() : 0);
                OnDone(false, Cues);
                return;
            }
            // A `cues` envelope — the third route and the third shape. `/scenes` answers
            // with a bare array, `/performances` wraps one in `performances`, this wraps
            // one in `cues`. Assuming the symmetry is how a parse ends up silently
            // reading an empty list off a perfectly good response.
            TSharedPtr<FJsonObject> Envelope;
            const TSharedRef<TJsonReader<TCHAR>> Reader =
                TJsonReaderFactory<TCHAR>::Create(Resp->GetContentAsString());
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid()
                || !Envelope->TryGetArrayField(TEXT("cues"), Rows) || !Rows)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("%s answered 200 but not with a running order"), *ListUrl);
                OnDone(false, Cues);
                return;
            }
            // Order preserved exactly as received, and that is load-bearing rather than
            // incidental: `list_performance_cues` selects `ORDER BY order_idx, rowid`
            // (backend/app/db.py), so the array IS the running order and its positions
            // are the indices a person types. Nothing here sorts or renumbers.
            Cues.Reserve(Rows->Num());
            for (const TSharedPtr<FJsonValue>& Row : *Rows)
            {
                const TSharedPtr<FJsonObject> Entry = Row.IsValid() ? Row->AsObject() : nullptr;
                FLoomaCue Cue;
                // No scene, no cue: a cue exists to fire a scene, so a row without one
                // could only ever be counted. Dropping it does shift the indices of
                // everything after it, which is the argument for keeping it — but a
                // placeholder that cannot be fired would shift them just as much the
                // moment somebody typed its number and got nothing.
                if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("scene_id"), Cue.SceneId)
                    || Cue.SceneId.IsEmpty())
                {
                    continue;
                }
                // Nullable by design — `add_cue` stores `clean_label or None` — so an
                // absent one is an unlabelled cue, not a broken row.
                Entry->TryGetStringField(TEXT("label"), Cue.Label);
                Cues.Add(MoveTemp(Cue));
            }
            OnDone(true, Cues);
        });
    Request->ProcessRequest();
}

void ULoomaSceneSyncSubsystem::LogCues()
{
    const FString PerformanceId = ActivePerformance.Id;
    const FString Pending = GetPendingPerformanceId();
    if (PerformanceId.IsEmpty())
    {
        UE_LOG(LogLoomaSync, Warning,
            TEXT("No running order to show: no `scene` frame has named a performance yet. ")
            TEXT("`Looma.Status` says whether the socket is up."));
        return;
    }
    if (!Pending.IsEmpty())
    {
        // Answered anyway, unlike OpenCue, but only after saying whose rail this is.
        // The list is real and reading it is harmless; what would be misleading is the
        // heading "the running order" over the room we are on our way out of.
        UE_LOG(LogLoomaSync, Warning,
            TEXT("A switch to '%s' is in flight, so what follows is the running order of '%s' — the ")
            TEXT("performance being LEFT. `Looma.Cue <index>` is refused until the new socket's ")
            TEXT("`scene` frame lands."),
            *Pending, *PerformanceId);
    }
    const FString ActiveScene = ActiveSceneId;
    FetchCues(PerformanceId, [PerformanceId, ActiveScene](bool bOk, const TArray<FLoomaCue>& Cues) {
        if (!bOk)
        {
            UE_LOG(LogLoomaSync, Warning, TEXT("...so there is no running order to show."));
            return;
        }
        if (Cues.Num() == 0)
        {
            // The common case today, and emphatically not a fault — every performance on
            // both backends has an empty rail because nobody has arranged one yet. Said
            // in a way that does not send somebody hunting for a bug: the pool and the
            // running order are separate lists, so a performance full of scenes and
            // empty of cues is an ordinary, working, unarranged performance.
            UE_LOG(LogLoomaSync, Display,
                TEXT("Performance '%s' has an empty running order — no scenes have been put on the ")
                TEXT("line yet. Nothing is wrong: the pool and the running order are different ")
                TEXT("lists, so it can hold scenes that are not on the rail. `Looma.Scenes` lists ")
                TEXT("what there is to open directly."),
                *PerformanceId);
            return;
        }
        int32 SceneWidth = 0;
        for (const FLoomaCue& Cue : Cues)
        {
            SceneWidth = FMath::Max(SceneWidth, Cue.SceneId.Len());
        }
        const int32 IndexWidth = FString::FromInt(Cues.Num() - 1).Len();
        UE_LOG(LogLoomaSync, Display, TEXT("Running order of performance '%s' — %d cue(s), 0-based:"),
            *PerformanceId, Cues.Num());
        int32 Marked = 0;
        for (int32 Index = 0; Index < Cues.Num(); ++Index)
        {
            const FLoomaCue& Cue = Cues[Index];
            const bool bIsActive = !ActiveScene.IsEmpty() && Cue.SceneId == ActiveScene;
            Marked += bIsActive ? 1 : 0;
            UE_LOG(LogLoomaSync, Display, TEXT("%s %s  %s  %s%s"),
                bIsActive ? TEXT("*") : TEXT(" "),
                *Pad(FString::FromInt(Index), IndexWidth),
                *Pad(Cue.SceneId, SceneWidth),
                Cue.Label.IsEmpty() ? TEXT("<no label>") : *Cue.Label,
                bIsActive ? TEXT("   (active scene)") : TEXT(""));
        }
        if (Marked > 1)
        {
            // The reprise case, said out loud rather than hidden by marking the first.
            // A scene may sit on the line more than once, and the wire moves by SCENE —
            // `openScene` carries a `sceneId` and the `scene` frame answers with one —
            // so nothing this client holds distinguishes two cues that name the same
            // scene. The marker is therefore "the active scene is here", not "we are at
            // this cue", and the difference is the user's to resolve.
            UE_LOG(LogLoomaSync, Display,
                TEXT("The active scene is on the line %d times (a reprise). Which of those cues is ")
                TEXT("live is not on the wire — the hub moves clients by scene, not by position — ")
                TEXT("so all of them are marked."),
                Marked);
        }
        else if (Marked == 0 && !ActiveScene.IsEmpty())
        {
            UE_LOG(LogLoomaSync, Display,
                TEXT("The active scene '%s' is not on this line. That is ordinary: a scene can be ")
                TEXT("opened straight from the pool without being in the running order."),
                *ActiveScene);
        }
        UE_LOG(LogLoomaSync, Display,
            TEXT("`Looma.Cue <index>` opens one; `Looma.Scenes` pairs these ids with their names."));
    });
}

void ULoomaSceneSyncSubsystem::OpenCue(int32 CueIndex)
{
    const FString Pending = GetPendingPerformanceId();
    if (!Pending.IsEmpty())
    {
        // Refused rather than raced. Both halves of firing a cue here would look
        // correct on their own — read the running order we are in, send an `openScene`
        // — while together they can open a scene from the room being left into the room
        // we land in, which is the one outcome nobody could explain afterwards.
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Not firing cue %d: a switch to '%s' is in flight, so the running order we would ")
            TEXT("read belongs to the performance being left and the socket may be in another one ")
            TEXT("by the time it arrives. Wait for the `scene` frame — `Looma.Performance` says ")
            TEXT("when it lands."),
            CueIndex, *Pending);
        return;
    }
    const FString PerformanceId = ActivePerformance.Id;
    if (PerformanceId.IsEmpty())
    {
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Not firing cue %d: no `scene` frame has named a performance, so there is no ")
            TEXT("running order to index into. `Looma.Status` says whether the socket is up."),
            CueIndex);
        return;
    }
    // Nothing to disclaim: this command did one thing and the outcome is about that
    // one thing. The post-switch caller passes a sentence here instead.
    ResolveAndOpenCue(PerformanceId, CueIndex, FString());
}

void ULoomaSceneSyncSubsystem::ResolveAndOpenCue(const FString& PerformanceId, int32 CueIndex,
    const FString& FailureNote)
{
    FetchCues(PerformanceId,
        [this, CueIndex, PerformanceId, FailureNote](bool bOk, const TArray<FLoomaCue>& Cues) {
            if (!bOk)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("...so cue %d cannot be resolved.%s"),
                    CueIndex, *FailureNote);
                return;
            }
            if (Cues.Num() == 0)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("Performance '%s' has no cues at all, so there is no cue %d to fire. ")
                    TEXT("Nothing is wrong with it — a running order is arranged separately from ")
                    TEXT("the pool, and this one has not been. `Looma.Scene <name-or-id>` opens a ")
                    TEXT("scene directly.%s"),
                    *PerformanceId, CueIndex, *FailureNote);
                return;
            }
            if (!Cues.IsValidIndex(CueIndex))
            {
                // The range, not a clamp. Clamping is how a performance shows the wrong
                // scene to a room, and it would hide the off-by-one this console is
                // probably being used to find.
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("There is no cue %d: performance '%s' has %d, numbered 0 to %d. ")
                    TEXT("`Looma.Cue` with no argument prints them.%s"),
                    CueIndex, *PerformanceId, Cues.Num(), Cues.Num() - 1, *FailureNote);
                return;
            }
            const FLoomaCue& Cue = Cues[CueIndex];
            // The index, the scene and the label together, because the index is what was
            // typed and the scene id is what every later line will call it — and a cue
            // fired by number should say what it turned out to be before it happens.
            UE_LOG(LogLoomaSync, Display, TEXT("Cue %d of '%s' is scene '%s'%s; opening it."),
                CueIndex, *PerformanceId, *Cue.SceneId,
                Cue.Label.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" ('%s')"), *Cue.Label));
            // An `openScene` and nothing else: the cue names a scene in the performance
            // this socket is already in, so there is no reconnect and no identity to
            // re-resolve.
            OpenScene(Cue.SceneId);
        });
}

void ULoomaSceneSyncSubsystem::SwitchPerformance(const FString& PerformanceId)
{
    const FString Wanted = PerformanceId.TrimStartAndEnd();
    if (Wanted.IsEmpty())
    {
        // Refused rather than treated as "unset the request and let the hub choose".
        // That state exists and is reachable — it is what a client that never asked is
        // in — but there is no console spelling for it (`Looma.Performance` with no
        // arguments is the report), and reconnecting someone into a room they did not
        // name is the one move this whole mechanism exists to make visible.
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Looma: a performance switch needs an id — nothing was sent, and this client stays ")
            TEXT("in %s."),
            ActivePerformance.Id.IsEmpty() ? TEXT("whichever room the hub gave it") : *ActivePerformance.Id);
        return;
    }
    // No early-out when Wanted already equals ActivePerformance.Id, though it looks
    // free. That id can be one socket out of date — nothing clears it on a drop — so
    // "you are already there" would be answered from stale state at exactly the moment
    // somebody types this to get unstuck, and asking for the room you are in is a
    // legitimate way to force a clean rejoin. A reconnect is the documented cost of
    // this command, not a surprise it should try to dodge.
    RequestedPerformanceId = Wanted;
    // A plain switch supersedes a parked jump, and clearing it HERE rather than in the
    // console command is what makes that true of every caller — including
    // SwitchPerformanceToCue, which calls through and then arms its own. Left behind, a
    // cue number typed a minute ago would fire into a room chosen since.
    PendingCueIndex = INDEX_NONE;
    UE_LOG(LogLoomaSync, Display,
        TEXT("Asking for performance '%s' — reconnecting, because a socket's performance is fixed at ")
        TEXT("`hello` and never changes for the life of the connection."),
        *Wanted);
    Reconnect();
}

void ULoomaSceneSyncSubsystem::SwitchPerformanceToCue(const FString& PerformanceId, int32 CueIndex)
{
    if (CueIndex < 0)
    {
        // Refused before the socket is touched, because this much IS knowable now: no
        // running order has a negative position, whatever the destination turns out to
        // hold. The RANGE cannot be known until the new rail is read, which is why only
        // this half is checked early — a reconnect that everybody in the room pays for
        // should not be spent proving something arithmetic.
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Not switching: cue indices are 0-based and cannot be negative, so %d names no cue ")
            TEXT("in any performance."),
            CueIndex);
        return;
    }
    // Through the plain switch, so there is one implementation of "ask for a
    // performance" — its empty-id refusal, its request bookkeeping and its reconnect —
    // and this adds only the parked intent. It also clears PendingCueIndex on the way
    // through, so arming after the call is what makes a second two-argument command
    // supersede the first rather than queue behind it.
    SwitchPerformance(PerformanceId);
    if (RequestedPerformanceId.IsEmpty())
    {
        // The switch was refused (an empty id), and it has already said so. Arming a
        // jump for a reconnect that is not happening would leave it to fire on whatever
        // reconnect came next.
        return;
    }
    PendingCueIndex = CueIndex;
    UE_LOG(LogLoomaSync, Display,
        TEXT("Cue %d will open once the new socket is confirmed in a performance — the running ")
        TEXT("order is read then, for where we actually land."),
        CueIndex);
}

void ULoomaSceneSyncSubsystem::ConfirmRequestedPerformance()
{
    if (!bAwaitingPerformanceConfirmation)
    {
        return;
    }
    // Answered, whatever the answer is. Left set, this would re-announce the switch on
    // every later `scene` frame — and every `openScene` reply is one.
    bAwaitingPerformanceConfirmation = false;
    // Taken and cleared together with the latch, for the same reason: this is the one
    // instant the parked jump is meaningful, and a copy left behind would fire again on
    // the next `scene` frame — which the jump itself is about to cause.
    const int32 Jump = PendingCueIndex;
    PendingCueIndex = INDEX_NONE;
    if (ActivePerformance.Id.IsEmpty())
    {
        // The frame named no performance, which the hub does not do. Nothing to compare
        // against, and nothing worth alarming a user about: the frame we did get is
        // still applied, and `Looma.Performance` will report the emptiness honestly.
        UE_LOG(LogLoomaSync, Verbose,
            TEXT("A `scene` frame answered our request for '%s' without naming a performance"),
            *SentPerformanceId);
        if (Jump != INDEX_NONE)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Cue %d was not opened: the frame that answered the switch named no ")
                TEXT("performance, so there is no running order to index into."),
                Jump);
        }
        return;
    }
    if (ActivePerformance.Id == SentPerformanceId)
    {
        UE_LOG(LogLoomaSync, Display, TEXT("Performance '%s' confirmed by the hub%s"),
            *ActivePerformance.Id,
            ActivePerformance.Name.IsEmpty()
                ? TEXT("")
                : *FString::Printf(TEXT(" — '%s'"), *ActivePerformance.Name));
        if (Jump != INDEX_NONE)
        {
            // The one path that fires. GetPendingPerformanceId is already empty — the
            // latch above cleared it — so the guard on `Looma.Cue` is not tripped, and
            // this is precisely the instant that guard was protecting: the switch has
            // resolved, so the rail about to be read is the one we are actually in.
            ResolveAndOpenCue(ActivePerformance.Id, Jump,
                FString::Printf(
                    TEXT(" The switch itself stands: this client is in '%s'."),
                    *ActivePerformance.Id));
        }
        return;
    }
    // NOT an error, and it must not read as one. The contract explicitly allows the hub
    // to place a client somewhere other than where it asked, and that possibility is the
    // entire reason the `performance` object is on the wire — a client that assumed it
    // got what it asked for could never detect the difference. But it must not pass
    // silently either: somebody typed an id and is now somewhere else, and the log is
    // where that gets said.
    UE_LOG(LogLoomaSync, Display,
        TEXT("Asked for performance '%s'; the hub put this client in '%s'%s instead. Not an error — ")
        TEXT("the hub decides, and it says so on the `scene` frame for exactly this reason."),
        *SentPerformanceId,
        *ActivePerformance.Id,
        ActivePerformance.Name.IsEmpty()
            ? TEXT("")
            : *FString::Printf(TEXT(" ('%s')"), *ActivePerformance.Name));
    if (Jump != INDEX_NONE)
    {
        // DROPPED, not applied to the room we ended up in. The number was chosen
        // against the running order of the performance the user named, and every
        // running order is a different show of a different length — so firing it here
        // would open a scene picked from a list they have never seen, in a room they
        // did not choose. Two surprises compounding, and the second one silent.
        //
        // The other reading — "cue 3 means the fourth thing that happens, wherever I
        // am" — is coherent, and it is what makes this worth stating rather than
        // assuming. It loses because the index is not a name: it means nothing without
        // the list it points into, and the user has not seen this one.
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Cue %d was not opened: it was chosen for '%s' and this client is in '%s', whose ")
            TEXT("running order is a different list. `Looma.Cue` prints this one."),
            Jump, *SentPerformanceId, *ActivePerformance.Id);
    }
}

void ULoomaSceneSyncSubsystem::HandleTerminalClose(int32 Code, const FString& Reason)
{
    TerminalCloseCode = Code;
    // Nothing armed, so Tick's countdown never reaches Connect(). This is the whole of
    // "terminal": the client rests DISCONNECTED and idle rather than looping, and only
    // an explicit act starts it again.
    ReconnectCooldown = 0.0f;
    // The switch is over however it ended. Left pending, `Looma.Status` and
    // `Looma.Performance` would go on announcing a switch that was refused.
    bAwaitingPerformanceConfirmation = false;
    // And so is any cue parked behind it. The socket it was waiting for will never
    // open, so the jump would otherwise sit armed until some unrelated reconnect
    // happened to fire it — the failure mode of every intent that outlives its reason.
    // Reported rather than dropped quietly: a two-part command deserves to be told
    // which parts did not happen, and only the first part's failure is announced below.
    const bool bHadJump = PendingCueIndex != INDEX_NONE;
    const int32 DroppedJump = PendingCueIndex;
    PendingCueIndex = INDEX_NONE;

    // What THIS socket asked for — the only id a refusal can be about. Deliberately not
    // cleared: see below.
    const FString Asked = SentPerformanceId;
    FString Explanation;
    if (Code == CloseMalformedHello)
    {
        Explanation = TEXT("the hub rejected our handshake as malformed, and retrying would send the ")
                      TEXT("identical `hello`. That makes it a bug in this plugin rather than ")
                      TEXT("something to wait out");
    }
    else if (Code == CloseKicked)
    {
        Explanation = Asked.IsEmpty()
            ? FString(TEXT("this identity is kicked from the performance the hub placed it in, and only ")
                      TEXT("a moderator can lift that"))
            : FString::Printf(
                TEXT("this identity is kicked from performance '%s', and only a moderator can lift that"),
                *Asked);
    }
    else
    {
        // Non-enumerating on purpose: "does not exist" and "private, and you are not a
        // member" are indistinguishable from the wire, exactly as they are through
        // `GET /performances/{id}`. So this names both and guesses neither — a message
        // that picked one would be inventing the half the contract refuses to disclose.
        Explanation = Asked.IsEmpty()
            ? FString(TEXT("the hub has no performance available to this identity"))
            : FString::Printf(
                TEXT("no performance '%s' is available to this identity — either it does not exist, or ")
                TEXT("it is private and this identity is not a member. The wire cannot tell those apart"),
                *Asked);
    }

    // The two doors out, named every time, because a client that stops and says nothing
    // more is a headset that looks broken — which is the failure this replaced the retry
    // loop with if the message is not carried properly.
    //
    // THE REQUEST IS KEPT. Clearing it was the alternative and it is worse: a 4403 is
    // explicitly temporary (a kick gets lifted) and a 4404 becomes a success the moment
    // somebody shares the performance, so retrying the same id is usually the right next
    // move — and dropping it would move the user somewhere they never asked for, quietly,
    // which is precisely the failure `_performance_info` was put on the wire to expose.
    // Walking back into the same refusal stays possible, but only by typing
    // `Looma.Reconnect` after being told in this line that it will.
    //
    // Warning and not Error, though this is the most final thing the plugin can say:
    // nothing else in the plugin uses Error, and a lone one would rank a refusal above a
    // backend that never comes up at all. Finality is carried by the words and by the
    // state, not by the verbosity.
    UE_LOG(LogLoomaSync, Warning,
        TEXT("Socket refused (%d %s) and NOT retrying: %s. %s"),
        Code, *Reason, *Explanation,
        Asked.IsEmpty()
            ? TEXT("`Looma.Reconnect` tries again; `Looma.Performance <id>` asks for a different room.")
            : TEXT("The request is kept, so `Looma.Reconnect` retries this same id; ")
              TEXT("`Looma.Performance <other-id>` goes somewhere else."));
    if (bHadJump)
    {
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Cue %d was dropped with it: it was waiting for a socket that will not open."),
            DroppedJump);
    }
}

// --- Connection control / diagnostics ----------------------------------------

void ULoomaSceneSyncSubsystem::Reconnect()
{
    ReconnectCooldown = 0.0f; // no waiting: the caller asked for it now
    UE_LOG(LogLoomaSync, Display, TEXT("Reconnecting to %s"), *GetSceneSyncUrl());
    // Closing our own socket clears its handlers, so nothing else will say we dropped —
    // and a UI bound to OnSyncDisconnected must not be left showing "connected".
    const bool bWasConnected = IsSyncConnected();
    Connect();
    if (bWasConnected)
    {
        OnSyncDisconnected.Broadcast();
    }
}

void ULoomaSceneSyncSubsystem::OnSettingsChanged()
{
    // Two knobs here need the socket's attention, and only when they actually move.
    // Every other one — LightIntensityScale, bBaseAlignModels, WebAssetPrefix — is read
    // where it is used and must never cost anyone a reconnect.
    if (GetSceneSyncUrl() != SocketUrl)
    {
        UE_LOG(LogLoomaSync, Display, TEXT("Backend moved to %s"), *GetRestBase());
        // A different backend can have different auth settings, and the old one's
        // answer is worse than none: it would leave a login screen drawn (or skipped)
        // on the authority of a server we no longer talk to. Drop to Unknown now — the
        // Unknown broadcast is what tells a UI to stop trusting what it is showing —
        // then ask the new address. The REST probe is not redundant with the socket
        // coming up: if the new address has no hub the socket never opens, while
        // /health may still answer perfectly well (a path-prefix typo does exactly
        // that).
        SetAuthState(ELoomaAuthState::Unknown);
        // And the session with it. A token minted by one backend means nothing at
        // another, so keeping it could only ever do one of two things: be rejected, or
        // be sent — a bearer belonging to a different server — to whoever now answers
        // at this address. Neither is worth the convenience of not retyping a password.
        //
        // No re-handshake asked of it: Reconnect() below is already doing one for the
        // address change, and letting ClearSession add a second would tear down the
        // socket the first had just opened.
        ClearSession(/*bRehandshake=*/false);
        // Reset the backoff and release the in-flight guard before re-probing: the
        // pending answer describes the old address, and the new one deserves to be
        // asked at once rather than at whatever interval the old one had backed off to.
        CancelHealthRetry();
        Reconnect();
        ProbeHealth(/*bLogDiagnostics=*/false);
        // Nothing below applies: Connect() has just re-read the display name too.
        return;
    }

    // The guest name reaches the hub only in a `hello`, and there is no message that
    // renames a socket already up — so applying an edit means reconnecting. Every other
    // knob in that panel takes effect live, so a name that silently did nothing until
    // some later reconnect would read as broken.
    const FString SuggestedName = ULoomaSceneSyncSettings::Get().GuestDisplayName.TrimStartAndEnd();
    if (SuggestedName == SentDisplayName)
    {
        return;
    }

    if (HasAuthToken())
    {
        // Deliberately nothing, and this is the interesting half. While we hold a
        // session the hub resolves identity from it and never consults the suggestion
        // (identity_from_websocket, backend/app/auth/local.py) — so a reconnect here
        // could not change the name by even one character. It would still cost every
        // other client in the room a leave and a join in their roster, which is the
        // real price of a reconnect; the GLBs are not (a login reconnect was measured
        // re-applying 33 nodes with zero meshes rebuilt). Pure churn for no effect.
        //
        // The edit is not lost. It sits in the settings and the next guest connection
        // picks it up — logging out reconnects, and Connect() re-reads it there.
        UE_LOG(LogLoomaSync, Log,
            TEXT("Guest Display Name changed, but this client is logged in — the hub takes the name from ")
            TEXT("the session, so it will apply on the next guest connection."));
        return;
    }

    UE_LOG(LogLoomaSync, Display, TEXT("Guest display name changed to '%s'; reconnecting to suggest it"),
        SuggestedName.IsEmpty() ? TEXT("<none>") : *SuggestedName);
    Reconnect();
}

FString ULoomaSceneSyncSubsystem::GetSceneSyncUrl() const
{
    // Same base, WebSocket scheme: http -> ws, https -> wss.
    FString Url = GetRestBase();
    if (Url.StartsWith(TEXT("https://")))
    {
        Url = TEXT("wss://") + Url.RightChop(8);
    }
    else if (Url.StartsWith(TEXT("http://")))
    {
        Url = TEXT("ws://") + Url.RightChop(7);
    }
    return Url + TEXT("/ws/scene");
}

FString ULoomaSceneSyncSubsystem::GetConnectionStatusText() const
{
    FString State;
    // First, because it outranks everything below and cannot coexist with it: Connect()
    // clears the code, so a set one means no socket is up or coming up. It also has to
    // be said instead of "DISCONNECTED", which reads as a state that resolves itself.
    // This one does not, and that is the whole point of it.
    if (TerminalCloseCode != 0)
    {
        State = FString::Printf(TEXT("REFUSED (%d), not retrying"), TerminalCloseCode);
    }
    else if (IsSyncConnected())
    {
        State = TEXT("CONNECTED");
    }
    else if (bConnecting)
    {
        State = TEXT("CONNECTING");
    }
    else if (ReconnectCooldown > 0.0f)
    {
        State = FString::Printf(TEXT("DISCONNECTED (retry in %.1fs)"), ReconnectCooldown);
    }
    else
    {
        State = Socket.IsValid() ? TEXT("DISCONNECTED") : TEXT("NO SOCKET");
    }

    // The socket's URL, not the settings' — after an edit they differ until the
    // reconnect lands, and the useful answer is where we are actually pointed.
    const FString HubUrl = SocketUrl.IsEmpty() ? GetSceneSyncUrl() : SocketUrl;
    const FString RestBase = GetRestBase();
    // The performance as its ID and not its name: this is a diagnostic, the id is the
    // key the hub, the REST paths and every other client agree on, and the name is the
    // one field of it that can legitimately be missing. `Looma.Performance` prints both.
    //
    // The two placeholders differ on purpose. `<unsaved>` is a scene the hub really has
    // us on that has never been saved; `<unknown>` is the absence of an answer, since
    // there is no performance the hub can put us in without naming its id.
    FString Performance = ActivePerformance.Id.IsEmpty() ? TEXT("<unknown>") : ActivePerformance.Id;
    // A switch in flight is printed BESIDE the confirmed id rather than in place of it,
    // for as long as the two can disagree. Nothing clears ActivePerformance on a drop,
    // so through the whole of a reconnect it names the room being left — and it looks
    // entirely valid while doing so, which is the most misleading kind of stale a status
    // line can be. Both facts are true; only together are they the answer.
    const FString Pending = GetPendingPerformanceId();
    if (!Pending.IsEmpty())
    {
        Performance += FString::Printf(TEXT(" (switching to '%s')"), *Pending);
    }
    return FString::Printf(
        TEXT("%s | hub %s | rest %s | auth %s | %d node(s), performance %s, scene %s | %d job(s)"),
        *State,
        *HubUrl,
        *RestBase,
        AuthStateText(AuthState),
        Tracked.Num(),
        *Performance,
        ActiveSceneId.IsEmpty() ? TEXT("<unsaved>") : *ActiveSceneId,
        Jobs.Num());
}

void ULoomaSceneSyncSubsystem::LogConnectionStatus()
{
    UE_LOG(LogLoomaSync, Display, TEXT("Looma Scene Sync: %s"), *GetConnectionStatusText());
    // The line above is free — it reports the socket and the auth state we already
    // hold. The probe is the half that talks to the backend, and typing `Looma.Status`
    // is the one case where its running commentary is the point.
    ProbeHealth(/*bLogDiagnostics=*/true);
}

// --- Auth discovery ----------------------------------------------------------

ELoomaAuthState ULoomaSceneSyncSubsystem::GetAuthState() const
{
    return AuthState;
}

bool ULoomaSceneSyncSubsystem::IsAuthStateKnown() const
{
    return AuthState != ELoomaAuthState::Unknown;
}

bool ULoomaSceneSyncSubsystem::IsAuthEnabled() const
{
    // Unknown answers false here *and* false from IsAuthStateKnown, so the pair
    // (known = false, enabled = false) is the only way "undecided" can present. A
    // caller that forgets the known-check gets the timid answer, not the confident
    // wrong one.
    return AuthState == ELoomaAuthState::EnabledRegistrationClosed
        || AuthState == ELoomaAuthState::EnabledRegistrationOpen;
}

bool ULoomaSceneSyncSubsystem::IsRegistrationEnabled() const
{
    return AuthState == ELoomaAuthState::EnabledRegistrationOpen;
}

void ULoomaSceneSyncSubsystem::SetAuthState(ELoomaAuthState NewState)
{
    if (AuthState == NewState)
    {
        return;
    }
    AuthState = NewState;
    if (AuthState != ELoomaAuthState::Unknown)
    {
        // Settled, so stop asking. Done here rather than at each of ProbeHealth's
        // success paths, which makes "the retry runs only while the state is Unknown"
        // true by construction instead of by three call sites agreeing.
        CancelHealthRetry();
    }
    // One line per actual change, not per probe: the probe runs unprompted and on a
    // retry loop, and a log entry each time would be pure noise.
    UE_LOG(LogLoomaSync, Log, TEXT("Backend auth: %s"), AuthStateText(AuthState));
    OnAuthStateChanged.Broadcast(AuthState);
}

void ULoomaSceneSyncSubsystem::ScheduleHealthRetry()
{
    if (IsAuthStateKnown())
    {
        return; // nothing left to ask
    }
    HealthProbeRetryDelay = (HealthProbeRetryDelay <= 0.0f)
        ? HealthRetryMinDelay
        : FMath::Min(HealthProbeRetryDelay * 2.0f, HealthRetryMaxDelay);
    HealthProbeCooldown = HealthProbeRetryDelay;
    UE_LOG(LogLoomaSync, Verbose, TEXT("Auth state still unknown; asking again in %.0fs"),
        HealthProbeCooldown);
}

void ULoomaSceneSyncSubsystem::CancelHealthRetry()
{
    HealthProbeCooldown = 0.0f;
    HealthProbeRetryDelay = 0.0f;
    bHealthProbeInFlight = false;
}

void ULoomaSceneSyncSubsystem::TickHealthRetry(float DeltaTime)
{
    if (HealthProbeCooldown <= 0.0f)
    {
        return;
    }
    HealthProbeCooldown -= DeltaTime;
    if (HealthProbeCooldown <= 0.0f)
    {
        HealthProbeCooldown = 0.0f;
        ProbeHealth(/*bLogDiagnostics=*/false);
    }
}

void ULoomaSceneSyncSubsystem::ProbeHealth(bool bLogDiagnostics)
{
    // Two jobs, one request: the auth-discovery probe (the `auth` block of /health,
    // added by HAM-172) and — when bLogDiagnostics is set — the `Looma.Status` triage.
    //
    // A socket can be down for three different reasons: nothing listening, the wrong
    // host, or the right host with the wrong path prefix. One REST call tells them
    // apart, but only if we check *what* answered: a reverse proxy that serves the web
    // app at the root answers /health with 200 and an index.html, which reads as
    // healthy while the hub URL points at nothing that speaks WebSocket. That triage
    // earns a paragraph in the log when a human asked for it, and is noise on every
    // editor launch — where the backend commonly is not up yet, and the socket layer
    // already says so once per retry. Hence the flag rather than two request bodies to
    // keep in step. The quiet path still logs, at Verbose, so
    // `-LogCmds="LogLoomaSync Verbose"` gets the whole story back.
    if (!bLogDiagnostics)
    {
        // Arm the retry on the way out, before the request even exists. Doing it here
        // rather than from the failure paths is what makes the schedule unconditional:
        // a request that is dropped without ever completing still leaves a re-ask
        // pending. A definitive answer cancels it (see SetAuthState).
        ScheduleHealthRetry();
        if (bHealthProbeInFlight)
        {
            return; // one at a time; the outstanding one will report or time out
        }
        bHealthProbeInFlight = true;
    }

    const FString RestBase = GetRestBase();
    const FString HealthUrl = RestBase + TEXT("/health");
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(HealthUrl);
    Request->SetVerb(TEXT("GET"));
    Request->SetTimeout(bLogDiagnostics ? HealthDiagnosticTimeout : BackgroundRequestTimeout);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [this, HealthUrl, RestBase, bLogDiagnostics](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            if (!bLogDiagnostics)
            {
                bHealthProbeInFlight = false;
            }
            // The settings may have moved the backend while this was in flight. What
            // that server said about auth describes a server we no longer talk to, and
            // recording it would clobber the pending answer from the new one.
            if (RestBase != GetRestBase())
            {
                UE_LOG(LogLoomaSync, Verbose, TEXT("Dropping a /health answer from %s — the backend moved to %s"),
                    *RestBase, *GetRestBase());
                return;
            }

            // Every failure path below lands on Unknown instead of guessing Disabled.
            // The guess is right against the default configuration, which is exactly
            // what makes it dangerous: it would walk a client into a scene it never
            // authenticated for, and turn a missing login into an unexplained 401
            // later, in another subsystem, far from here.
            if (!bOk || !Resp.IsValid())
            {
                SetAuthState(ELoomaAuthState::Unknown);
                if (bLogDiagnostics)
                {
                    UE_LOG(LogLoomaSync, Warning, TEXT("Backend %s unreachable — is the backend running, ")
                                                  TEXT("and is the Backend URL right?"), *HealthUrl);
                }
                else
                {
                    UE_LOG(LogLoomaSync, Verbose,
                        TEXT("Health probe of %s failed; auth state stays unknown for now"), *HealthUrl);
                }
                return;
            }
            const int32 Code = Resp->GetResponseCode();
            const FString Body = Resp->GetContentAsString();
            const FString Excerpt = Body.Left(120).Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" "));
            if (Code >= 300)
            {
                SetAuthState(ELoomaAuthState::Unknown);
                if (bLogDiagnostics)
                {
                    UE_LOG(LogLoomaSync, Warning, TEXT("Backend %s answered %d: %s"), *HealthUrl, Code, *Excerpt);
                }
                else
                {
                    UE_LOG(LogLoomaSync, Verbose, TEXT("Backend %s answered %d; auth state stays unknown"),
                        *HealthUrl, Code);
                }
                return;
            }

            TSharedPtr<FJsonObject> Json;
            const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Body);
            FString Status;
            if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid() &&
                Json->TryGetStringField(TEXT("status"), Status))
            {
                int32 Assets = 0;
                Json->TryGetNumberField(TEXT("assets"), Assets);
                // Only a body that is recognisably *this* backend's /health gets this
                // far — the same test the triage below turns on — so the auth state can
                // never be taken from whatever else happens to serve that address.
                SetAuthState(ReadAuthState(Json));
                // The backend has just proved it answers, on a connection slot that has
                // this instant come free — the best moment on offer to check a token
                // restored from disk. Hung off the probe rather than given a retry loop
                // of its own, because the probe already keeps asking until the backend
                // is reachable; and guarded on *provisional*, so a session that has
                // been confirmed once is not re-checked on every later probe.
                if (IsIdentityProvisional())
                {
                    RefreshIdentity();
                }
                if (bLogDiagnostics)
                {
                    UE_LOG(LogLoomaSync, Display, TEXT("Backend %s OK (%d): status %s, %d asset(s), auth %s"),
                        *HealthUrl, Code, *Status, Assets, AuthStateText(AuthState));
                }
                return;
            }
            SetAuthState(ELoomaAuthState::Unknown);
            if (bLogDiagnostics)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("Backend %s answered %d but not with the backend's /health JSON — something else is ")
                    TEXT("serving that address (a web app, usually). If the API sits behind a path prefix, ")
                    TEXT("the Backend URL must include it, e.g. %s/api. Got: %s"),
                    *HealthUrl, Code, *RestBase, *Excerpt);
            }
            else
            {
                UE_LOG(LogLoomaSync, Verbose,
                    TEXT("Backend %s answered %d with something other than /health JSON; auth stays unknown"),
                    *HealthUrl, Code);
            }
        });
    Request->ProcessRequest();
}

// --- Auth: the session -------------------------------------------------------

FLoomaIdentity ULoomaSceneSyncSubsystem::GetIdentity() const
{
    return CurrentIdentity;
}

bool ULoomaSceneSyncSubsystem::HasAuthToken() const
{
    return !AuthToken.IsEmpty();
}

FString ULoomaSceneSyncSubsystem::GetIdentityText() const
{
    // Holding no token with nothing resolved is not an identity to describe, it is the
    // absence of one, and "<no name> (unknown)" described it in a way that reads like a
    // bug. This is the normal state of a fresh launch and of every moment after a logout,
    // so it gets a plain sentence. Distinct from the provisional case below, which DOES
    // hold a token and does have a name to show.
    if (!HasAuthToken() && CurrentIdentity.Kind == ELoomaIdentityKind::Unknown
        && CurrentIdentity.DisplayName.IsEmpty())
    {
        return TEXT("not signed in");
    }

    const TCHAR* KindText = TEXT("unknown");
    switch (CurrentIdentity.Kind)
    {
    case ELoomaIdentityKind::Guest:
        KindText = TEXT("guest");
        break;
    case ELoomaIdentityKind::User:
        KindText = TEXT("user");
        break;
    default:
        break;
    }
    // Never the token, and not the user id either: an id adds nothing a reader of a log
    // can use, and is one more field to redact out of a pasted bug report.
    //
    // The token note distinguishes the provisional window explicitly, because
    // "alice (unknown)" on its own reads like a contradiction to whoever ran
    // `Looma.Whoami` two seconds after launch — the name is off the disk and the kind
    // has not been confirmed yet, and the suffix is what says so.
    const TCHAR* TokenText = TEXT("");
    if (IsIdentityProvisional())
    {
        TokenText = TEXT(" [restored session, not yet validated]");
    }
    else if (HasAuthToken())
    {
        TokenText = TEXT(" [session token held]");
    }
    return FString::Printf(TEXT("%s (%s%s)%s"),
        CurrentIdentity.DisplayName.IsEmpty() ? TEXT("<no name>") : *CurrentIdentity.DisplayName,
        KindText,
        CurrentIdentity.bIsAdmin ? TEXT(", admin") : TEXT(""),
        TokenText);
}

bool ULoomaSceneSyncSubsystem::IsIdentityProvisional() const
{
    // A token but no confirmed identity. Only the restore path can produce this pair:
    // a login sets both at once, and clearing drops both.
    return HasAuthToken() && CurrentIdentity.Kind == ELoomaIdentityKind::Unknown;
}

void ULoomaSceneSyncSubsystem::ApplyAuthHeader(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request) const
{
    if (AuthToken.IsEmpty())
    {
        // Not an error: every route serves guests, so an unauthenticated request is a
        // guest request. Callers must not have to branch on this.
        return;
    }
    // Same-origin, checked here rather than at the call sites, because being the only
    // function that attaches the token is what makes the check worth anything: a bearer
    // *cannot* leave for another host, however the URL was assembled. Not theoretical —
    // the image download's URL comes off a generation job and through
    // ResolveBackendUrl, which forwards an absolute URL untouched, so a job naming
    // some other host would otherwise be handed this session.
    //
    // A prefix match ALONE is not enough, and the counter-example is the default
    // configuration rather than an exotic one. With a base of "http://127.0.0.1:8000",
    // the URL
    //
    //     http://127.0.0.1:8000@evil.com/whatever
    //
    // starts with the base and is not our backend at all: everything before the "@" is
    // userinfo, and the host is evil.com. "http://127.0.0.1:8000.evil.com/" and
    // "http://127.0.0.1:80001/" slip through the same hole. A base that happens to
    // carry a path prefix ("https://host/api") is accidentally safe because the "/api"
    // forces the boundary; nothing should depend on the operator having configured one.
    //
    // So: the base, then a path boundary. The next character must be "/" — which "@",
    // "." and a digit are not — or the URL must be the base exactly. That also rejects
    // "https://host/apifoo" for a base of "https://host/api". Strict on purpose: it
    // turns away a bare-base query ("…:8000?x=1") too, which is a legal URL but one no
    // call site here builds, so the strictness costs nothing and saves a judgement call.
    //
    // Deliberately a delimiter check and not a URL parse. Every call site builds its URL
    // as GetRestBase() + "/...", so this states the invariant we actually want — "this
    // URL was built from our base" — rather than approximating it by comparing
    // scheme/host/port, and it inherits no parser's opinions about default ports, IPv6
    // brackets or empty authorities. It is also strictly stronger than a host
    // comparison, because it pins the path prefix too.
    //
    // GetRestBase() guarantees a non-empty base with no trailing slash
    // (NormalizeRestBase), which is what makes the boundary character unambiguous and
    // the index below in range.
    const FString RestBase = GetRestBase();
    const FString& Url = Request->GetURL();
    // Case-insensitive, as StartsWith defaults to: scheme and host are case-insensitive
    // per RFC 3986, and the only thing it loosens is the *path* prefix's case, which
    // cannot change which host receives the token.
    const bool bSameOrigin = Url.StartsWith(RestBase)
        && (Url.Len() == RestBase.Len() || Url[RestBase.Len()] == TEXT('/'));
    if (!bSameOrigin)
    {
        UE_LOG(LogLoomaSync, Verbose, TEXT("Withholding the bearer from %s — not the configured backend"),
            *Url);
        return;
    }
    Request->SetHeader(AuthHeaderName, AuthHeaderBearerPrefix + AuthToken);
}

void ULoomaSceneSyncSubsystem::ApplyAuthHeader(TMap<FString, FString>& UpgradeHeaders) const
{
    if (AuthToken.IsEmpty())
    {
        return;
    }
    // No origin check to make here: the only caller is Connect(), and the URL it is
    // about to open is GetSceneSyncUrl() — derived from GetRestBase() by construction.
    UpgradeHeaders.Add(AuthHeaderName, AuthHeaderBearerPrefix + AuthToken);
}

void ULoomaSceneSyncSubsystem::ApplyClientIdHeader(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request) const
{
    // The same value the `hello` carries, which is the whole point of it: it is what
    // ties a guest's one-shot POSTs to the socket the room roster already knows them
    // by. No same-origin guard, unlike ApplyAuthHeader — this is a name seed and not a
    // credential, so there is nothing here for another host to be handed.
    Request->SetHeader(ClientIdHeaderName, ClientId);
}

void ULoomaSceneSyncSubsystem::ApplyAuthToken(const TSharedRef<FJsonObject>& Hello) const
{
    if (AuthToken.IsEmpty())
    {
        return;
    }
    Hello->SetStringField(TEXT("token"), AuthToken);
}

void ULoomaSceneSyncSubsystem::SetSession(const FString& Token, const FLoomaIdentity& NewIdentity)
{
    AuthToken = Token;
    // Bumped before the broadcast, so a handler that fires off a request from inside
    // OnIdentityChanged captures the serial it will actually be checked against.
    ++SessionSerial;
    SetIdentity(NewIdentity);
}

void ULoomaSceneSyncSubsystem::AdoptSession(const FString& Token, const FLoomaIdentity& NewIdentity)
{
    SetSession(Token, NewIdentity);
    // Persisted here and deleted in ClearSession — one write site and one delete site,
    // which is what makes logout, an expired token and a backend change all covered
    // without a fourth caller to remember.
    SaveSession();
    // The hub resolves an identity once, from the handshake, so a socket that came up
    // as a guest stays a guest until it is re-opened — this is what makes a login
    // visible to the other clients in the room rather than only to us.
    //
    // Cheap, despite appearances: a reconnect re-sends the whole `scene`, but every
    // node still in it keeps the actor it already has, so ApplyModel's
    // `Context.ModelUrl != LoadedModelUrl` guard holds and not one GLB is re-fetched.
    // Last, after the identity broadcast, so a UI handler reads the new identity before
    // the connection events for the new socket arrive.
    Reconnect();
}

void ULoomaSceneSyncSubsystem::ClearSession(bool bRehandshake)
{
    const bool bHadSession = HasAuthToken();
    // Unconditionally, whatever bRehandshake says: that flag is about the socket, not
    // about the disk. A session dropped because the backend moved must not be left on
    // disk to be restored on the next launch.
    DeleteSavedSession();
    // Unknown, not a guest identity we invent. We cannot name our own guest identity:
    // the hub derives `Guest-xxxxxx` from our WS clientId and only it knows the result,
    // and GET /auth/me is no help — its HTTP path mints a FRESH random guest seed on
    // every call with no session (see the CLIENT_ID_HEADER note in
    // backend/app/auth/provider.py), so it would name us something no other client
    // sees in the roster. Unknown is the honest answer until the socket says otherwise.
    SetSession(FString(), FLoomaIdentity());

    // Only worth a reconnect if we were actually presenting a session: dropping a
    // token we never had changes nothing about how the hub sees this socket, and
    // reconnecting on it would churn the room for no reason (a `Looma.Logout` from a
    // guest, say). RefreshIdentity's dead-token path deliberately does want this — we
    // are connected as an account the backend has stopped recognising.
    if (bRehandshake && bHadSession)
    {
        Reconnect();
    }
}

void ULoomaSceneSyncSubsystem::SetIdentity(const FLoomaIdentity& NewIdentity)
{
    if (CurrentIdentity == NewIdentity)
    {
        return;
    }
    CurrentIdentity = NewIdentity;
    UE_LOG(LogLoomaSync, Log, TEXT("Identity: %s"), *GetIdentityText());
    OnIdentityChanged.Broadcast(CurrentIdentity);
}

FString ULoomaSceneSyncSubsystem::GetSessionFilePath() const
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), SessionFileDir, SessionFileName);
}

void ULoomaSceneSyncSubsystem::SaveSession() const
{
    const FString Path = GetSessionFilePath();
    if (AuthToken.IsEmpty())
    {
        return; // nothing to write; ClearSession is what removes the file
    }

    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    // The backend that minted it, so the loader can refuse to hand a token to a
    // different one. Same reasoning OnSettingsChanged applies while running; without it
    // here, a restart would be a way to smuggle a token from backend A to backend B.
    Root->SetStringField(SessionFieldBackend, GetRestBase());
    Root->SetStringField(SessionFieldToken, AuthToken);
    // The display name and nothing else of the identity. Not the user id, which no
    // caller needs before validation, and emphatically not `is_admin`: a cached
    // capability read back off the disk is a capability an attacker with write access
    // to Saved/ could grant themselves in a UI. The backend re-checks it on every admin
    // route regardless, so the only thing that would achieve is a misleading screen.
    Root->SetStringField(SessionFieldDisplayName, CurrentIdentity.DisplayName);

    FString Text;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Root, Writer);

    // SaveStringToFile will not create the directory itself.
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
    // ForceUTF8WithoutBOM rather than AutoDetect: a display name can be non-ASCII, and
    // a deterministic encoding is one fewer thing for the loader to be surprised by.
    // It round-trips — FFileHelper::BufferToString decodes a buffer with no BOM through
    // FUTF8ToTCHAR_Convert, so a UTF-8 name comes back as it went in.
    if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        // Best-effort. The live session is unaffected — this only costs the *next*
        // launch a login — so it is a warning and not a failure handed back to a caller.
        UE_LOG(LogLoomaSync, Warning, TEXT("Could not save the session to %s; this login will not persist"),
            *Path);
    }
}

void ULoomaSceneSyncSubsystem::DeleteSavedSession() const
{
    const FString Path = GetSessionFilePath();
    if (!IFileManager::Get().FileExists(*Path))
    {
        return;
    }
    // Quiet: a delete that fails is worth one line, not the file manager's own error.
    if (!IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true))
    {
        UE_LOG(LogLoomaSync, Warning, TEXT("Could not delete the saved session at %s"), *Path);
        return;
    }
    UE_LOG(LogLoomaSync, Verbose, TEXT("Deleted the saved session at %s"), *Path);
}

void ULoomaSceneSyncSubsystem::LoadSavedSession()
{
    const FString Path = GetSessionFilePath();
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        return; // no saved session, which is the ordinary case
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
    FString Backend;
    FString Token;
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() ||
        !Root->TryGetStringField(SessionFieldBackend, Backend) ||
        !Root->TryGetStringField(SessionFieldToken, Token) || Token.IsEmpty())
    {
        // Unreadable, or missing something we require. Note what is not logged: no
        // excerpt of the file, because the file is the one place a token is at rest.
        UE_LOG(LogLoomaSync, Warning, TEXT("Ignoring the saved session at %s — it could not be read"), *Path);
        DeleteSavedSession();
        return;
    }

    // A token is scoped to the backend that issued it. Both sides of this comparison
    // are NormalizeRestBase output, so an exact match is the right test — and a
    // mismatch is discarded rather than kept, because a token for a backend we are no
    // longer pointed at is not going to become useful again by sitting there.
    if (Backend != GetRestBase())
    {
        UE_LOG(LogLoomaSync, Log,
            TEXT("Discarding the saved session: it belongs to %s and the backend is now %s"),
            *Backend, *GetRestBase());
        DeleteSavedSession();
        return;
    }

    // Provisional: the token, and the display name as a *label*, with Kind left Unknown
    // because nothing has yet said this token is still live. The backend's session TTL
    // is 30 days (backend/app/auth/local.py), so it usually is — but "usually" is not
    // something to encode in the identity. GET /auth/me promotes it, or clears it; see
    // IsIdentityProvisional.
    FLoomaIdentity Provisional;
    Root->TryGetStringField(SessionFieldDisplayName, Provisional.DisplayName);

    // Straight to SetSession, not AdoptSession: there is no socket to re-handshake yet
    // (Initialize calls this before Connect, deliberately), and writing the file back
    // that we have this second read would be pointless.
    SetSession(Token, Provisional);
    UE_LOG(LogLoomaSync, Log, TEXT("Restored a saved session for %s (unvalidated so far)"),
        Provisional.DisplayName.IsEmpty() ? TEXT("<no name>") : *Provisional.DisplayName);
}

void ULoomaSceneSyncSubsystem::RequestLogin(const FString& Username, const FString& Password,
    TFunction<void(bool bSuccess, const FLoomaIdentity& Identity, const FString& Error)> OnComplete)
{
    const FString LoginUrl = GetRestBase() + TEXT("/auth/login");

    // The serial advances when a login is *sent*, not only when one is adopted, so that
    // of two overlapping logins the later request wins whichever answer arrives first.
    // Without it, two rapid logins as different accounts could land out of order and
    // leave us holding the older account's token — and RefreshIdentity already carries
    // this guard, so login lacking it would read as an oversight rather than a decision.
    // A superseded login still reports, as a failure: the Blueprint node has a latent
    // action waiting on exactly one of its two pins.
    ++SessionSerial;
    const uint32 Serial = SessionSerial;

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("username"), Username);
    Body->SetStringField(TEXT("password"), Password);
    FString BodyText;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&BodyText);
    FJsonSerializer::Serialize(Body, Writer);

    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(LoginUrl);
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    // The one request in the plugin whose body holds a password. It is never logged,
    // and BodyText goes out of scope as soon as the request owns its copy; keeping the
    // credential off the wire entirely is TLS's job, not this function's.
    Request->SetContentAsString(BodyText);
    // Interactive: a person typed this and is watching. Long enough for an Argon2id
    // verify (deliberately slow) plus a contended connection pool, short enough that a
    // dead backend says so while they still care.
    Request->SetTimeout(InteractiveRequestTimeout);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [this, LoginUrl, OnComplete, Serial](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            // Every path below reports through here exactly once, and no failure path
            // touches the session: a rejected login leaves the socket up, the scene
            // loaded, and whatever identity we already had still ours.
            const auto Fail = [&OnComplete](const FString& Message) {
                UE_LOG(LogLoomaSync, Warning, TEXT("%s"), *Message);
                if (OnComplete)
                {
                    OnComplete(false, FLoomaIdentity(), Message);
                }
            };

            if (Serial != SessionSerial)
            {
                // Another login was sent, or the session was dropped, after this one
                // went out. Adopting now would install a token the user has already
                // moved on from.
                Fail(TEXT("Login superseded by a later request."));
                return;
            }

            if (!bOk || !Resp.IsValid())
            {
                Fail(FString::Printf(TEXT("Login failed: %s is unreachable."), *LoginUrl));
                return;
            }

            const int32 Code = Resp->GetResponseCode();
            if (Code >= 300)
            {
                // The backend's own `detail` string, read only on the error paths — so
                // the one response in this API that does carry a token is never picked
                // over for something to print.
                const FString Detail = ErrorDetail(Resp);
                if (Code == 401)
                {
                    // One message for a wrong password and an account that does not
                    // exist alike. The backend raises the same InvalidCredentials, with
                    // the same wording, after the same amount of work — it verifies
                    // against a dummy hash when there is no row, so even a timing probe
                    // learns nothing (backend/app/auth/local.py). Telling the two apart
                    // here would hand back the account-enumeration oracle it went to
                    // that trouble to deny.
                    Fail(Detail.IsEmpty()
                            ? FString(TEXT("Login failed: those credentials were not accepted."))
                            : Detail);
                    return;
                }
                if (Code == 403)
                {
                    // AuthDisabled, not a bad password: the operator switched accounts
                    // off, so no credentials would work and retyping is not the remedy.
                    // Worth its own message for exactly that reason.
                    Fail(Detail.IsEmpty()
                            ? FString(TEXT("Login failed: this backend has authentication disabled."))
                            : Detail);
                    return;
                }
                Fail(FString::Printf(TEXT("Login failed: POST /auth/login answered %d."), Code));
                return;
            }

            TSharedPtr<FJsonObject> Json;
            const TSharedRef<TJsonReader<TCHAR>> Reader =
                TJsonReaderFactory<TCHAR>::Create(Resp->GetContentAsString());
            const TSharedPtr<FJsonObject>* IdentityObj = nullptr;
            FString Token;
            if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid() ||
                !Json->TryGetStringField(TEXT("token"), Token) || Token.IsEmpty() ||
                !Json->TryGetObjectField(TEXT("identity"), IdentityObj) || !IdentityObj)
            {
                // A success we cannot read is a failure, not a session. Note what is
                // *not* in the message: no excerpt of the body, because this is the one
                // response in the API that carries a token.
                Fail(FString::Printf(
                    TEXT("Login failed: the backend answered %d without a usable identity and token."), Code));
                return;
            }

            const FLoomaIdentity Identity = LoomaParseIdentity(*IdentityObj);
            // Adopt before reporting, so a caller reading GetIdentity() / HasAuthToken()
            // on the success pin sees the session rather than the state before it.
            AdoptSession(Token, Identity);
            UE_LOG(LogLoomaSync, Display, TEXT("Logged in: %s"), *GetIdentityText());
            if (OnComplete)
            {
                OnComplete(true, CurrentIdentity, FString());
            }
        });
    Request->ProcessRequest();
}

void ULoomaSceneSyncSubsystem::Logout()
{
    // Forget first, ask second, and never make the forgetting depend on the answer.
    // POST /auth/logout is 204 whatever happens — revoking an expired, unknown or
    // foreign token is a no-op by contract (backend/app/auth/routes.py) — so there is
    // no failure a user needs told about. And a logout that kept the token because a
    // packet went missing would leave this client authenticated behind a UI saying it
    // is not, which is much the worse of the two bugs. The token is copied out first
    // so the revoke can still name what it is revoking.
    const FString RevokedToken = AuthToken;
    ClearSession();
    // Said out loud either way: a console command that does nothing visible reads as
    // broken, and "there was nothing to log out of" is itself the answer someone
    // running this wanted.
    UE_LOG(LogLoomaSync, Display, TEXT("Logged out%s"),
        RevokedToken.IsEmpty() ? TEXT(" (there was no session to revoke)") : TEXT(""));

    if (RevokedToken.IsEmpty())
    {
        return; // nothing was ever issued, so there is nothing to revoke
    }

    const FString LogoutUrl = GetRestBase() + TEXT("/auth/logout");
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(LogoutUrl);
    Request->SetVerb(TEXT("POST"));
    // The header is set from the local copy rather than through ApplyAuthHeader,
    // because AuthToken is already empty by now — putting it back so a helper could
    // read it out again would reopen the very window this ordering closes.
    Request->SetHeader(AuthHeaderName, AuthHeaderBearerPrefix + RevokedToken);
    // Background: nothing waits on the revoke, and a short budget would only convert
    // "the pool was busy" into "the token was never revoked".
    Request->SetTimeout(BackgroundRequestTimeout);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            // Best-effort by contract: nothing here may touch local state, and there is
            // nothing for a user to do about it, so this is Verbose and not a Warning.
            const bool bRevoked = bOk && Resp.IsValid() && Resp->GetResponseCode() < 300;
            UE_LOG(LogLoomaSync, Verbose, TEXT("Logout revoke %s"),
                bRevoked ? TEXT("acknowledged") : TEXT("did not land (the token expires on its own)"));
        });
    Request->ProcessRequest();
}

void ULoomaSceneSyncSubsystem::RefreshIdentity()
{
    const FString MeUrl = GetRestBase() + TEXT("/auth/me");
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(MeUrl);
    Request->SetVerb(TEXT("GET"));
    // Background, and the tier matters most here: a spurious timeout must never read as
    // evidence about the token. It does not — the failure path below leaves the
    // identity untouched — but the shorter the budget, the more often that path is
    // taken for no reason.
    Request->SetTimeout(BackgroundRequestTimeout);
    ApplyAuthHeader(Request);

    const bool bSentWithToken = HasAuthToken();
    const uint32 Serial = SessionSerial;
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [this, MeUrl, bSentWithToken, Serial](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            if (Serial != SessionSerial)
            {
                // The session moved while this was in flight — a login, a logout, or a
                // backend change. This answer describes a session we are no longer in,
                // and the classic bug it would cause is a guest `/auth/me` sent before
                // a login landing after it and demoting the account identity.
                UE_LOG(LogLoomaSync, Verbose, TEXT("Dropping a stale /auth/me answer"));
                return;
            }
            if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 300)
            {
                // Leave the identity exactly as it was. A refresh that did not happen
                // tells us nothing about who we are, and overwriting a known identity
                // with Unknown on a dropped packet is a client that logs itself out
                // every time the network hiccups.
                UE_LOG(LogLoomaSync, Warning, TEXT("Could not refresh identity from %s (%d)"), *MeUrl,
                    Resp.IsValid() ? Resp->GetResponseCode() : 0);
                return;
            }

            TSharedPtr<FJsonObject> Json;
            const TSharedRef<TJsonReader<TCHAR>> Reader =
                TJsonReaderFactory<TCHAR>::Create(Resp->GetContentAsString());
            if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("%s answered 200 but not with an identity"), *MeUrl);
                return;
            }

            const FLoomaIdentity Fresh = LoomaParseIdentity(Json);
            // THE thing to get right about this route: it never answers 401. An
            // unauthenticated caller gets a guest identity with a 200
            // (backend/app/auth/routes.py), so the status code says nothing whatsoever
            // about whether our token is alive — only `kind` does. A client that read
            // the code here would trust a revoked token until something else refused it.
            if (Fresh.Kind == ELoomaIdentityKind::User)
            {
                SetIdentity(Fresh);
                // Write the confirmed name back over the cached one. Without this a
                // server-side rename would show stale on every launch for as long as
                // the token lives — 30 days — because nothing else ever rewrites the
                // file for an unchanged token.
                SaveSession();
                return;
            }
            if (bSentWithToken)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("Our session token no longer resolves to an account — the backend answered as a ")
                    TEXT("guest, so the session has been revoked or expired. Dropping it."));
                ClearSession();
                return;
            }
            // A guest answer to a request that carried no token: expected, and not
            // adoptable. `/auth/me`'s HTTP path mints a FRESH random `Guest-xxxxxx` per
            // call when there is no session, so this name matches nothing any other
            // client sees in the roster — writing it down would invent an identity
            // rather than learn one. Nothing here to learn; leave what we have alone.
            UE_LOG(LogLoomaSync, Verbose,
                TEXT("%s answered as a guest and we sent no token — identity left unchanged"), *MeUrl);
        });
    Request->ProcessRequest();
}

// --- Inbound -----------------------------------------------------------------

void ULoomaSceneSyncSubsystem::OnRawMessage(const FString& Text)
{
    TSharedPtr<FJsonObject> Msg;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Msg) || !Msg.IsValid())
    {
        return;
    }
    const FString Type = Msg->GetStringField(TEXT("type"));

    bApplyingRemote = true;
    if (Type == TEXT("scene"))
    {
        HandleScene(Msg);
    }
    else if (Type == TEXT("spawn"))
    {
        HandleSpawn(Msg);
    }
    else if (Type == TEXT("despawn"))
    {
        HandleDespawn(Msg);
    }
    else if (Type == TEXT("transform"))
    {
        HandleTransform(Msg);
    }
    else if (Type == TEXT("reparent"))
    {
        HandleReparent(Msg);
    }
    else if (Type == TEXT("patch"))
    {
        HandlePatch(Msg);
    }
    else if (Type == TEXT("generation"))
    {
        HandleGeneration(Msg);
    }
    else if (Type == TEXT("sceneError"))
    {
        HandleSceneError(Msg);
    }
    else if (Type == TEXT("clients"))
    {
        HandleClients(Msg);
    }
    else if (Type == TEXT("selection"))
    {
        HandleSelection(Msg);
    }
    else
    {
        // Not an error — the hub may speak messages a newer backend added — but
        // silence here is how "the plugin ignores half the protocol" hid for two
        // format versions.
        UE_LOG(LogLoomaSync, Verbose, TEXT("Ignoring unknown message type '%s'"), *Type);
    }
    bApplyingRemote = false;
}

void ULoomaSceneSyncSubsystem::HandleScene(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Msg->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
    {
        // Nothing is read before this guard, including the performance, and that is the
        // consistent answer rather than an oversight: a frame with no `nodes` is not a
        // scene frame we can act on, so it is refused WHOLE. `sceneId` and the
        // performance then keep their previous values together, which is the only pair
        // of states that stays coherent — adopting a workspace off a frame whose
        // document we had just declined would leave us reporting a room we refused to
        // enter. They go stale together and the next real frame replaces both. It
        // should not fire at all: `scene_message` always builds `nodes`
        // (backend/app/sync.py), so this catches a truncated or foreign frame.
        return;
    }
    // `sceneId` is null for an unsaved working scene, and TryGet leaves the old value
    // in place on a null — so clear it first.
    ActiveSceneId.Reset();
    Msg->TryGetStringField(TEXT("sceneId"), ActiveSceneId);

    // The workspace, read beside `sceneId` because it is the same statement made at the
    // same moment, and because this frame is the ONLY carrier of it — no other message
    // names the performance, so what is not taken here is not had at all.
    //
    // REPLACED WHOLE, cleared first, like `sceneId` and for a sharper reason. A null
    // `name` beside a valid `id` is a statement rather than a gap: `_performance_info`
    // reads `db.get_performance(id) or {}`, so it means the row has been deleted out
    // from under this socket. The alternative — clear only what the frame omits, and so
    // keep the last name we knew — would print a plausible name for a workspace that no
    // longer exists, which is the exact class of silent disagreement this object was put
    // on the wire to prevent, not a smaller version of it. The id survives the deletion
    // and stays the truth about where we are, so (id, no name) is precisely the state
    // worth holding. A frame carrying no `performance` at all leaves the whole thing
    // empty, which reads as "not told" — the honest answer for a frame that did not.
    ActivePerformance = FLoomaPerformance();
    const TSharedPtr<FJsonObject>* Performance = nullptr;
    if (Msg->TryGetObjectField(TEXT("performance"), Performance) && Performance && Performance->IsValid())
    {
        (*Performance)->TryGetStringField(TEXT("id"), ActivePerformance.Id);
        (*Performance)->TryGetStringField(TEXT("name"), ActivePerformance.Name);
        (*Performance)->TryGetStringField(TEXT("visibility"), ActivePerformance.Visibility);
    }
    // Now that the confirmed value is in hand, settle what this socket asked for against
    // it. Here rather than at the end of the function because it is about the frame's
    // header and not its document: whether the nodes apply cleanly has no bearing on
    // which room we are in.
    ConfirmRequestedPerformance();

    // The hub owns the live scene: this is the whole document, sent on connect and
    // whenever someone activates or clears a scene. So it *replaces* what we hold
    // rather than merging into it — anything the hub no longer has, we no longer have.
    TSet<FString> Incoming;
    Incoming.Reserve(Nodes->Num());
    for (const TSharedPtr<FJsonValue>& Value : *Nodes)
    {
        const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
        FString NodeId;
        if (Node.IsValid() && Node->TryGetStringField(TEXT("id"), NodeId) && !NodeId.IsEmpty())
        {
            Incoming.Add(NodeId);
        }
    }

    TArray<FString> Stale;
    for (const TPair<FString, FLoomaTrackedActor>& Pair : Tracked)
    {
        if (!Incoming.Contains(Pair.Key))
        {
            Stale.Add(Pair.Key);
        }
    }
    for (const FString& NodeId : Stale)
    {
        DropNode(NodeId);
    }

    UpsertNodes(*Nodes);

    UE_LOG(LogLoomaSync, Log, TEXT("Scene%s: %d node(s) applied, %d dropped"),
        ActiveSceneId.IsEmpty() ? TEXT(" (unsaved)") : *FString::Printf(TEXT(" '%s'"), *ActiveSceneId),
        Nodes->Num(), Stale.Num());
}

void ULoomaSceneSyncSubsystem::HandleSceneError(const TSharedPtr<FJsonObject>& Msg)
{
    // A FRAME, not a close: this socket is fine and only the last thing it asked for is
    // not, unlike the handshake's 4400/4403/4404 which all mean start over
    // (docs/scene-format.md, "Choosing a scene (HAM-185)"; `_send_scene_error` in
    // backend/app/sync.py). So there is nothing to tear down, nothing to reconnect and
    // nothing to unwind — we are still on whatever scene we were on, and the next
    // message will work. Logging it *is* the handling.
    FString Reason;
    Msg->TryGetStringField(TEXT("reason"), Reason);
    FString Message;
    Msg->TryGetStringField(TEXT("message"), Message);
    // `sceneId` is nullable on this frame — a blank `openScene` is refused carrying the
    // id it did not have — so the miss is expected rather than malformed.
    FString SceneId;
    Msg->TryGetStringField(TEXT("sceneId"), SceneId);
    // Logged, never switched on. The `reason` vocabulary is backend/app/scenes.py's and
    // is open by design (`not_found` and `read_only` are simply the ones seen so far),
    // so a client branching on a closed set would silently mishandle the next one the
    // backend adds, in the one place it most needs to say something. Every sender today
    // is a person at a console or a Blueprint call, and the useful answer to both is the
    // backend's own sentence.
    UE_LOG(LogLoomaSync, Warning, TEXT("Scene request refused (%s) for scene %s: %s"),
        Reason.IsEmpty() ? TEXT("<no reason>") : *Reason,
        SceneId.IsEmpty() ? TEXT("<none>") : *SceneId,
        Message.IsEmpty() ? TEXT("<no message>") : *Message);
}

void ULoomaSceneSyncSubsystem::HandleSpawn(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Msg->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
    {
        return;
    }
    // Structural ops are re-broadcast to every client *including the sender*, so this
    // also sees our own spawns come back normalised. UpsertNode is idempotent.
    UpsertNodes(*Nodes);
}

void ULoomaSceneSyncSubsystem::UpsertNodes(const TArray<TSharedPtr<FJsonValue>>& Nodes)
{
    // One pass is enough: the hub orders spawns parents-first, even when the sender
    // listed them the other way round.
    for (const TSharedPtr<FJsonValue>& Value : Nodes)
    {
        UpsertNode(Value.IsValid() ? Value->AsObject() : nullptr);
    }
    ResolvePendingParents();
}

void ULoomaSceneSyncSubsystem::UpsertNode(const TSharedPtr<FJsonObject>& Node)
{
    if (!Node.IsValid())
    {
        return;
    }
    FString NodeId;
    if (!Node->TryGetStringField(TEXT("id"), NodeId) || NodeId.IsEmpty())
    {
        return; // a node with no id cannot be addressed, parented or transformed
    }

    // `parent` is null for a root, which TryGet reports as absent — an empty string.
    FString ParentId;
    Node->TryGetStringField(TEXT("parent"), ParentId);

    // **`t` is parent-local**, not a world pose. The axis/unit conversion is per node
    // and unchanged; only its meaning is (see LoomaWireConvert.h).
    const TSharedPtr<FJsonObject>* TField = nullptr;
    const FTransform Local =
        Node->TryGetObjectField(TEXT("t"), TField) ? LoomaWireToUe(*TField) : FTransform::Identity;

    const TArray<TSharedPtr<FJsonValue>>* ComponentArray = nullptr;
    Node->TryGetArrayField(TEXT("components"), ComponentArray);
    const FLoomaNodeComponents Components = LoomaParseComponents(ComponentArray);

    FString Name;
    Node->TryGetStringField(TEXT("name"), Name);

    ALoomaSyncedActor* Actor = FindSyncedActor(NodeId);
    const bool bFresh = Actor == nullptr;
    if (bFresh)
    {
        Tracked.Remove(NodeId); // a stale entry whose actor was garbage collected

        UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
        if (!World)
        {
            return;
        }
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        // Spawned at the origin: the pose is set below, by ApplyParent, once we know
        // what it is relative to.
        Actor = World->SpawnActor<ALoomaSyncedActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
        if (!Actor)
        {
            return;
        }
        Actor->Id = NodeId;
        Actor->OnDestroyed.AddDynamic(this, &ULoomaSceneSyncSubsystem::OnSyncedActorDestroyed);

        FLoomaTrackedActor Entry;
        Entry.Actor = Actor;
        Tracked.Add(NodeId, Entry);
        // A node arriving may be one a remote client already claims — a `selection`
        // legitimately races the `spawn` that created its node — so the border has to be
        // worked out again now that there is finally something to mark.
        MarkBordersDirty();
    }

    Actor->DisplayName = Name.IsEmpty() ? NodeId : Name;
#if WITH_EDITOR
    Actor->SetActorLabel(Actor->DisplayName);
#endif
    Actor->ApplyComponents(Components, MakeRenderContext(Components));
    ApplyParent(*Actor, ParentId, Local, /*bSnap=*/true);

    if (FLoomaTrackedActor* Entry = Tracked.Find(NodeId))
    {
        // Seed the outbound diff with the pose we just applied, so mirroring a remote
        // spawn doesn't read as local motion on the next tick.
        Entry->LastSent = Actor->GetLocalTransform();
        Entry->bMoving = false;
        Entry->StillFrames = 0;
    }

    if (bFresh)
    {
        UE_LOG(LogLoomaSync, Log, TEXT("Node '%s' (%s)%s: %s at local %s"),
            *Actor->DisplayName, *NodeId,
            ParentId.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" under %s"), *ParentId),
            Components.IsEmpty() ? TEXT("empty (transform only)") : *FString::Printf(TEXT("%s%s%s"),
                Components.bHasModel ? TEXT("model ") : TEXT(""),
                Components.bHasMesh ? TEXT("mesh ") : TEXT(""),
                Components.bHasLight ? TEXT("light") : TEXT("")),
            *Local.GetLocation().ToString());
    }
}

void ULoomaSceneSyncSubsystem::ApplyParent(ALoomaSyncedActor& Actor, const FString& ParentId,
    const FTransform& Local, bool bSnap)
{
    // Record the id even when it cannot be resolved: the hub guarantees parents-first
    // and turns a spawn naming an unknown parent into a root, so this is belt and
    // braces — the node becomes a root now, and ResolvePendingParents picks it up if
    // the parent does turn up later.
    Actor.ParentId = ParentId;

    ALoomaSyncedActor* Parent = ParentId.IsEmpty() ? nullptr : FindSyncedActor(ParentId);
    if (Parent && Parent != &Actor)
    {
        if (Actor.GetAttachParentActor() != Parent)
        {
            Actor.AttachToActor(Parent, FAttachmentTransformRules::KeepRelativeTransform);
        }
    }
    else if (Actor.GetAttachParentActor() != nullptr)
    {
        Actor.DetachFromActor(FDetachmentTransformRules::KeepRelativeTransform);
    }

    // Set the pose *after* attaching, because it is relative to whatever we just
    // attached to. UE composes parent scale x child relative location the same way
    // three.js does, so this is the same world pose the browser shows.
    Actor.SetRemoteTarget(Local, bSnap);
}

void ULoomaSceneSyncSubsystem::ResolvePendingParents()
{
    for (const TPair<FString, FLoomaTrackedActor>& Pair : Tracked)
    {
        ALoomaSyncedActor* Actor = Pair.Value.Actor.Get();
        if (!Actor || Actor->ParentId.IsEmpty() || Actor->GetAttachParentActor() != nullptr)
        {
            continue;
        }
        if (ALoomaSyncedActor* Parent = FindSyncedActor(Actor->ParentId))
        {
            // Its transform is already the parent-local one, so attaching keeping the
            // relative transform reinterprets the same numbers under the parent —
            // which is exactly what they always meant.
            UE_LOG(LogLoomaSync, Verbose, TEXT("Node '%s' found parent '%s' late — attaching"),
                *Actor->Id, *Actor->ParentId);
            Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepRelativeTransform);
        }
    }
}

void ULoomaSceneSyncSubsystem::DropNode(const FString& NodeId)
{
    FLoomaTrackedActor Entry;
    if (!Tracked.RemoveAndCopyValue(NodeId, Entry))
    {
        return;
    }
    // Its claim stays in the ledger — nothing retracts a claim but its owner — but the
    // primitive carrying the border is going away, so the marked set needs rebuilding.
    MarkBordersDirty();
    if (ALoomaSyncedActor* Actor = Entry.Actor.Get())
    {
        Actor->Destroy(); // bApplyingRemote suppresses the despawn echo
    }
}

FLoomaNodeRenderContext ULoomaSceneSyncSubsystem::MakeRenderContext(const FLoomaNodeComponents& Components) const
{
    const ULoomaSceneSyncSettings& Settings = ULoomaSceneSyncSettings::Get();
    FLoomaNodeRenderContext Context;
    Context.LightIntensityScale = Settings.LightIntensityScale;
    Context.bBaseAlignModels = Settings.bBaseAlignModels;
    if (Components.bHasModel && !Components.Model.AssetId.IsEmpty())
    {
        // Rebuilt from assetId, never taken from the component's `url`: that url is the
        // browser's /api-proxied relative path, which a native client cannot use.
        Context.ModelUrl = FString::Printf(TEXT("%s/static/%s.glb"), *GetRestBase(), *Components.Model.AssetId);
    }
    return Context;
}

FString ULoomaSceneSyncSubsystem::MakeWebAssetUrl(const FString& AssetId) const
{
    // Backend-relative and proxy-prefixed, because this is for the browser, not for
    // us: "/api/static/chair_01.glb". The datalake convention is <assetId>.glb — the
    // same assumption this plugin already makes when it fetches a GLB of its own.
    const FString& WebAssetPrefix = ULoomaSceneSyncSettings::Get().WebAssetPrefix;
    if (AssetId.IsEmpty() || WebAssetPrefix.IsEmpty())
    {
        return FString();
    }
    return FString::Printf(TEXT("%s/static/%s.glb"), *WebAssetPrefix, *AssetId);
}

void ULoomaSceneSyncSubsystem::HandleDespawn(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
    if (!Msg->TryGetArrayField(TEXT("ids"), Ids) || !Ids)
    {
        return;
    }
    // The cascade to descendants is already expanded by the hub: a client deletes the
    // one node the user picked, and everyone else is told its children went too.
    for (const TSharedPtr<FJsonValue>& Value : *Ids)
    {
        if (Value.IsValid())
        {
            DropNode(Value->AsString());
        }
    }
}

void ULoomaSceneSyncSubsystem::HandleTransform(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Msg->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
    {
        return;
    }
    bool bTransient = false;
    Msg->TryGetBoolField(TEXT("transient"), bTransient);

    for (const TSharedPtr<FJsonValue>& Value : *Nodes)
    {
        const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
        if (!Node.IsValid())
        {
            continue;
        }
        FString NodeId;
        Node->TryGetStringField(TEXT("id"), NodeId);
        FLoomaTrackedActor* Entry = Tracked.Find(NodeId);
        const TSharedPtr<FJsonObject>* TField = nullptr;
        if (!Entry || !Node->TryGetObjectField(TEXT("t"), TField))
        {
            continue; // an id we don't know yet is dropped — spawns arrive parents-first
        }
        const FTransform Target = LoomaWireToUe(*TField);
        if (ALoomaSyncedActor* Actor = Entry->Actor.Get())
        {
            Actor->SetRemoteTarget(Target, /*bSnap=*/!bTransient);
        }
        // Remote-driven motion must not re-broadcast: the diff cache tracks the
        // remote target, and the diff skips actors still easing toward one.
        Entry->LastSent = Target;
        Entry->bMoving = false;
        Entry->StillFrames = 0;
    }
}

void ULoomaSceneSyncSubsystem::HandleReparent(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Msg->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
    {
        return;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Nodes)
    {
        const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
        if (!Node.IsValid())
        {
            continue;
        }
        FString NodeId;
        if (!Node->TryGetStringField(TEXT("id"), NodeId))
        {
            continue;
        }
        ALoomaSyncedActor* Actor = FindSyncedActor(NodeId);
        if (!Actor)
        {
            continue;
        }
        FString ParentId;
        Node->TryGetStringField(TEXT("parent"), ParentId);

        // The message carries the node's *new* parent-local pose, worked out
        // pose-preservingly by whoever moved the edge — so the object stays where it
        // was on screen instead of jumping into its new parent's frame.
        const TSharedPtr<FJsonObject>* TField = nullptr;
        const FTransform Local = Node->TryGetObjectField(TEXT("t"), TField)
            ? LoomaWireToUe(*TField)
            : Actor->GetLocalTransform();
        ApplyParent(*Actor, ParentId, Local, /*bSnap=*/true);

        if (FLoomaTrackedActor* Entry = Tracked.Find(NodeId))
        {
            Entry->LastSent = Actor->GetLocalTransform();
            Entry->bMoving = false;
            Entry->StillFrames = 0;
        }
    }
}

void ULoomaSceneSyncSubsystem::HandlePatch(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Msg->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
    {
        return;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Nodes)
    {
        const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
        if (!Node.IsValid())
        {
            continue;
        }
        FString NodeId;
        if (!Node->TryGetStringField(TEXT("id"), NodeId))
        {
            continue;
        }
        ALoomaSyncedActor* Actor = FindSyncedActor(NodeId);
        if (!Actor)
        {
            continue;
        }

        // A patch sets a node's *own* fields: its name and its components. Parent moves
        // are `reparent`, poses are `transform`, and `id` is not editable at all.
        FString Name;
        if (Node->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
        {
            Actor->DisplayName = Name;
#if WITH_EDITOR
            Actor->SetActorLabel(Name);
#endif
        }

        // A component edit arrives as the node's **whole** components array — there are
        // no per-component ids, by design. Absent means "not touched"; an empty array
        // means "clear them", which is why TryGetArrayField's result matters and not
        // just the array's length.
        const TArray<TSharedPtr<FJsonValue>>* ComponentArray = nullptr;
        if (Node->TryGetArrayField(TEXT("components"), ComponentArray))
        {
            const FLoomaNodeComponents Components = LoomaParseComponents(ComponentArray);
            Actor->ApplyComponents(Components, MakeRenderContext(Components));
        }
    }
}

ALoomaSyncedActor* ULoomaSceneSyncSubsystem::FindSyncedActor(const FString& NodeId) const
{
    if (NodeId.IsEmpty())
    {
        return nullptr;
    }
    const FLoomaTrackedActor* Entry = Tracked.Find(NodeId);
    return Entry ? Entry->Actor.Get() : nullptr;
}

void ULoomaSceneSyncSubsystem::OnSyncedActorDestroyed(AActor* DestroyedActor)
{
    ALoomaSyncedActor* Actor = Cast<ALoomaSyncedActor>(DestroyedActor);
    if (!Actor || Actor->Id.IsEmpty())
    {
        return;
    }
    const bool bWasTracked = Tracked.Remove(Actor->Id) > 0;
    MarkBordersDirty();
    if (bWasTracked && !bApplyingRemote)
    {
        // Just the one node: the hub expands the cascade to its descendants and hands
        // the full list back to everyone, us included.
        TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
        Msg->SetStringField(TEXT("type"), TEXT("despawn"));
        TArray<TSharedPtr<FJsonValue>> Ids;
        Ids.Add(MakeShared<FJsonValueString>(Actor->Id));
        Msg->SetArrayField(TEXT("ids"), Ids);
        SendJson(Msg);
    }
}

// --- Outbound (subsystem tick: reconnect + motion diff) -----------------------

void ULoomaSceneSyncSubsystem::Tick(float DeltaTime)
{
    // Before anything else, and regardless of connection state: a handle created
    // last frame for an already-known job has had a frame to be bound.
    FlushPendingHandleReplays();

    // Ahead of the reconnect early-out below, deliberately. An unreachable backend is
    // the very case the re-probe exists for, and it is also the case where
    // ReconnectCooldown is permanently armed — counting the probe down after that
    // `return` would mean never counting it down at all.
    TickHealthRetry(DeltaTime);

    // Also ahead of the reconnect early-out, and for the same kind of reason: half of
    // this is about the local selection rather than the socket, and a reconnect backoff
    // is exactly when it must keep working. Its own send half is gated on connectivity
    // internally. Ordering against the pose diff below is a free choice — an id the hub
    // does not hold yet is passed straight through and becomes drawable when the node
    // arrives, precisely because "a selection legitimately races the spawn that created
    // the node".
    TickSelection();
    // After TickSelection, because a local selection change in this same frame is one
    // of the things the borders subtract — running it first would draw one frame of a
    // remote border on a node we had just taken.
    TickBorders();

    if (ReconnectCooldown > 0.0f)
    {
        ReconnectCooldown -= DeltaTime;
        if (ReconnectCooldown <= 0.0f)
        {
            Connect();
        }
        return;
    }
    if (IsSyncConnected())
    {
        TickOutbound(DeltaTime);
    }
}

// --- Local selection (outbound `selection`) -----------------------------------

void ULoomaSceneSyncSubsystem::SetLocalSelection(const TArray<ALoomaSyncedActor*>& Actors)
{
    // Rebuilt wholesale rather than diffed in: the wire carries a whole set, so the
    // canonical local operation is the same shape, and every other entry point below
    // reads-modifies-writes through here. Nothing is sent from this call — see
    // TickSelection for why the send is coalesced into the tick.
    LocalSelection.Reset(Actors.Num());
    for (ALoomaSyncedActor* Actor : Actors)
    {
        // A node with no id never went through the hub — an ALoomaSyncedActor someone
        // dropped into the map by hand — so there is nothing the wire could name it by.
        // Dropped here rather than at send time so the stored set is only ever things
        // that are actually reportable, and AddUnique keeps a caller passing the same
        // actor twice from putting it on the wire twice.
        if (Actor && !Actor->Id.IsEmpty())
        {
            LocalSelection.AddUnique(TWeakObjectPtr<ALoomaSyncedActor>(Actor));
        }
    }
}

void ULoomaSceneSyncSubsystem::SelectNode(ALoomaSyncedActor* Actor)
{
    if (!Actor || Actor->Id.IsEmpty())
    {
        return;
    }
    LocalSelection.AddUnique(TWeakObjectPtr<ALoomaSyncedActor>(Actor));
}

void ULoomaSceneSyncSubsystem::DeselectNode(ALoomaSyncedActor* Actor)
{
    if (!Actor)
    {
        return;
    }
    LocalSelection.Remove(TWeakObjectPtr<ALoomaSyncedActor>(Actor));
}

void ULoomaSceneSyncSubsystem::ClearSelection()
{
    // Deliberately not an early-out when already empty. The send is the point of this
    // call: `{"ids": []}` is what clears our borders in every other client, and there
    // is no teardown message that would do it for us. The diff decides whether it
    // actually goes out, which is the one place that judgement belongs.
    LocalSelection.Reset();
}

TArray<ALoomaSyncedActor*> ULoomaSceneSyncSubsystem::GetLocalSelection() const
{
    TArray<ALoomaSyncedActor*> Result;
    Result.Reserve(LocalSelection.Num());
    for (const TWeakObjectPtr<ALoomaSyncedActor>& Weak : LocalSelection)
    {
        if (ALoomaSyncedActor* Actor = Weak.Get())
        {
            Result.Add(Actor);
        }
    }
    return Result;
}

bool ULoomaSceneSyncSubsystem::IsNodeSelected(ALoomaSyncedActor* Actor) const
{
    return Actor != nullptr && LocalSelection.Contains(TWeakObjectPtr<ALoomaSyncedActor>(Actor));
}

TArray<FString> ULoomaSceneSyncSubsystem::GetLocalSelectionIds() const
{
    // The const twin of CollectSelectionIds: same answer, without the compaction, so a
    // Blueprint or a console command can ask without mutating anything.
    TArray<FString> Ids;
    Ids.Reserve(LocalSelection.Num());
    for (const TWeakObjectPtr<ALoomaSyncedActor>& Weak : LocalSelection)
    {
        const ALoomaSyncedActor* Actor = Weak.Get();
        if (Actor && !Actor->Id.IsEmpty())
        {
            Ids.AddUnique(Actor->Id);
        }
    }
    Ids.Sort();
    return Ids;
}

TArray<FString> ULoomaSceneSyncSubsystem::CollectSelectionIds()
{
    // Compact first: an actor destroyed while selected is no longer selected, and
    // deriving the ids here rather than storing them is what makes that true with no
    // destruction hook and no separate pruning pass. The resulting change is picked up
    // by the ordinary diff, so a destroyed node also *reports* its own deselection.
    LocalSelection.RemoveAll([](const TWeakObjectPtr<ALoomaSyncedActor>& Weak) {
        const ALoomaSyncedActor* Actor = Weak.Get();
        return Actor == nullptr || Actor->Id.IsEmpty();
    });
    return GetLocalSelectionIds();
}

void ULoomaSceneSyncSubsystem::TickSelection()
{
    // Derived once and used by both halves. This also compacts away destroyed actors,
    // which is itself one of the two ways the selection changes — see
    // CollectSelectionIds.
    const TArray<FString> Ids = CollectSelectionIds();

    // --- Local truth: has our selection changed? -----------------------------
    // Compared against state rather than driven from the setters, because a setter is
    // only one of the two ways this set moves; an actor destroyed while selected leaves
    // it with nobody calling anything. A detector catches both, and coalesces a gesture
    // that touches the selection several times in one frame into one event.
    //
    // Gated on neither connectivity nor the send diff. Both gates were wrong in
    // opposite directions: gating on the socket goes quiet during a reconnect, which a
    // login or a logout now causes; gating on the send diff fires on reconnect when
    // only the hub's knowledge changed.
    if (Ids != LastNotifiedSelectionIds)
    {
        LastNotifiedSelectionIds = Ids;
        // Our selection is subtracted from every remote border, so it moving changes what
        // they draw even though the room itself has not moved.
        MarkBordersDirty();
        OnLocalSelectionChanged.Broadcast(Ids);
    }

    // --- The wire: does the hub need telling? --------------------------------
    // The send baseline must only ever record a send that actually happened. SendJson
    // drops a message when the socket is down, so continuing while disconnected would
    // move LastSentSelectionIds to a value the hub was never told — and the diff would
    // then suppress the very message the reconnect needs. (bForceSelectionSend would
    // rescue it, but only by accident.)
    if (!IsSyncConnected())
    {
        return;
    }
    // The diff, so an idle scene sends nothing — the whole reason this is affordable to
    // evaluate every frame. bForceSelectionSend is the one thing a diff cannot know:
    // that the hub has forgotten what it was last told.
    if (!bForceSelectionSend && Ids == LastSentSelectionIds)
    {
        return;
    }
    bForceSelectionSend = false;
    LastSentSelectionIds = Ids;

    TArray<TSharedPtr<FJsonValue>> IdValues;
    IdValues.Reserve(Ids.Num());
    for (const FString& Id : Ids)
    {
        IdValues.Add(MakeShared<FJsonValueString>(Id));
    }

    TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
    Msg->SetStringField(TEXT("type"), TEXT("selection"));
    // `ids` and nothing else. The hub stamps `clientId` and `color` from its own
    // presence table and discards anything a client puts in them — a border is a claim
    // about who is working on what, so we could not wear another client's colour or
    // file a selection under their name even by trying. (SendJson adds its usual
    // `origin`, which the hub's selection handler does not read.)
    Msg->SetArrayField(TEXT("ids"), IdValues);
    SendJson(Msg);
}

#if WITH_EDITOR
void ULoomaSceneSyncSubsystem::OnEditorSelectionChanged(UObject* SelectionObject)
{
    // The payload is deliberately unused, and that is a decision rather than laziness.
    // It is the USelection *container* upcast to UObject*, not the object that changed,
    // and USelection::NoteUnknownSelectionChanged broadcasts it as nullptr outright — so
    // it cannot be trusted either to identify what moved or to say which selection set
    // moved. Reading the state is the only reliable answer, and it is the state we want
    // anyway: the wire carries a whole set.
    (void)SelectionObject;

    if (!GEditor)
    {
        return;
    }
    // Ours, not "the PIE world" in general: two PIE instances share this one static
    // event and hold nodes with the same ids, so without this each subsystem would
    // report the other's selection as its own.
    const UWorld* OurWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
    if (!OurWorld)
    {
        return;
    }

    TArray<ALoomaSyncedActor*> Candidates;
    if (USelection* ActorSelection = GEditor->GetSelectedActors())
    {
        // Class-filters and null-checks for us (USelection::GetSelectedObjects<T>).
        ActorSelection->GetSelectedObjects<ALoomaSyncedActor>(Candidates);
    }

    TArray<ALoomaSyncedActor*> Selected;
    Selected.Reserve(Candidates.Num());
    for (ALoomaSyncedActor* Actor : Candidates)
    {
        if (Actor && Actor->GetWorld() == OurWorld)
        {
            Selected.Add(Actor);
        }
    }

    // Whatever survived the filter, INCLUDING nothing. Selecting a stray level actor
    // that is not a synced node therefore clears our claim rather than being ignored,
    // and that is the honest reading of two available ones: the user has stopped
    // working on the node they had, and a border nobody ever retracts is a false claim
    // left in everyone else's viewport — precisely what the contract's "`[]` is a real
    // message" rule exists to prevent. The convenient reading (keep the old selection
    // because this event "was not about us") would leave that border until the user
    // happened to click a synced node again. Same call as every other entry point, so
    // there is one implementation and one diff; the honest reading also turns out to
    // need no special case at all.
    SetLocalSelection(Selected);
}
#endif // WITH_EDITOR

void ULoomaSceneSyncSubsystem::TickOutbound(float DeltaTime)
{
    SinceLastTransientSend += DeltaTime;
    const bool bMaySendTransient = SinceLastTransientSend >= TransientInterval;

    TArray<FString> Reparented;
    TArray<FString> Moved;
    TArray<FString> Settled;

    for (auto It = Tracked.CreateIterator(); It; ++It)
    {
        FLoomaTrackedActor& Entry = It.Value();
        ALoomaSyncedActor* Actor = Entry.Actor.Get();
        if (!Actor)
        {
            It.RemoveCurrent();
            continue;
        }
        if (Actor->IsActorBeingDestroyed())
        {
            // On its way out: its despawn is already on the wire (OnSyncedActorDestroyed
            // fires inside Destroy), and the detaching and re-posing UE does on the way
            // down is teardown, not an edit anyone made.
            continue;
        }
        if (Actor->HasRemoteTarget())
        {
            continue; // remote-driven right now — never echo it back
        }

        // --- Attachment, before pose -----------------------------------------
        // `ParentId` is written only by ApplyParent, which only the inbound paths
        // reach, so it *is* "the parent the hub last told us" — the baseline to diff
        // the attachment against, with no second copy of the truth to keep in step.
        // The equal case, which is every node on every other tick, costs a pointer
        // walk and one string compare (an empty-string test for a root).
        AActor* const AttachedTo = Actor->GetAttachParentActor();
        ALoomaSyncedActor* const WireParent = WireParentNode(AttachedTo);

        // Attached to something that is not a node — decided on HAM-160: the node stays
        // a **root** on the wire and reports its **world** pose, so the browser draws it
        // where it visibly is, which is the only honest reading available. The
        // attachment is kept here and simply not described: a root whose world pose the
        // level happens to drive still reports that pose correctly, because WirePose
        // reads it in world space. (One asymmetry to know about: a node moved *out of*
        // another node onto a stray actor does send `reparent: null`, and the hub's echo
        // of that runs ApplyParent, which detaches it here too.)
        //
        // Edge-triggered off the tracked entry, so this warns once per attachment
        // rather than once per tick.
        const bool bForeignParent = AttachedTo != nullptr && WireParent == nullptr;
        if (bForeignParent != Entry.bForeignParent)
        {
            Entry.bForeignParent = bForeignParent;
            if (bForeignParent)
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("Node '%s' is attached to '%s', which is not a synced node — the wire has no id ")
                    TEXT("for it, so a parent edge cannot be expressed. Reporting the node as a root at ")
                    TEXT("its world pose; attach it to another synced actor if the edge itself should sync."),
                    *Actor->Id, *AttachedTo->GetName());
            }
        }

        const bool bParentChanged = WireParent
            ? WireParent->Id != Actor->ParentId
            : !Actor->ParentId.IsEmpty();
        if (bParentChanged)
        {
            // A detach nobody asked for: the old parent was destroyed here, and
            // AActor::Destroy detaches its children on the way out. Its despawn is
            // already sent and the hub cascades that to this node, so reporting the
            // orphan as re-parented to the root is a message about a node that is
            // about to vanish. (The actor itself is not being destroyed, so the guard
            // above does not catch this — the parent's entry is simply gone.)
            const bool bOrphaned = !WireParent && !FindSyncedActor(Actor->ParentId);

            // Re-seat the baseline exactly as inbound HandleReparent does, whether or
            // not we report it. UE attached keeping the world pose, so the node's
            // relative transform moved without the node moving: leave that in
            // LastSent and the very next diff sends it as a `transform`, which the hub
            // relays verbatim under the node's *old* parent. That spurious message —
            // not the missing reparent — is what corrupts the stored scene.
            Actor->ParentId = WireParent ? WireParent->Id : FString();
            Entry.LastSent = WirePose(*Actor);
            Entry.bMoving = false;
            Entry.StillFrames = 0;
            if (!bOrphaned)
            {
                Reparented.Add(It.Key());
            }
            continue;
        }

        // Parent-local, like the wire: a child dragged inside its parent must report
        // its own motion, and a child carried *by* its parent must report none.
        const FTransform Current = WirePose(*Actor);
        const bool bChanged = !Current.Equals(Entry.LastSent, /*Tolerance=*/0.01f);
        if (bChanged)
        {
            Entry.bMoving = true;
            Entry.StillFrames = 0;
            if (bMaySendTransient)
            {
                Entry.LastSent = Current;
                Moved.Add(It.Key());
            }
        }
        else if (Entry.bMoving && ++Entry.StillFrames >= StillFramesForFinal)
        {
            Entry.bMoving = false;
            Entry.LastSent = Current;
            Settled.Add(It.Key());
        }
    }

    // Structure first: a pose in the same tick belongs to whatever hierarchy this
    // establishes. (Nothing re-parented above is in Moved or Settled — its baseline
    // was re-seated — but a *sibling* pose could be, and the ordering is free.)
    if (Reparented.Num() > 0)
    {
        SendReparent(Reparented);
    }
    if (Moved.Num() > 0)
    {
        SinceLastTransientSend = 0.0f;
        SendTransforms(Moved, /*bTransient=*/true);
    }
    if (Settled.Num() > 0)
    {
        SendTransforms(Settled, /*bTransient=*/false);
    }
}

void ULoomaSceneSyncSubsystem::SendTransforms(const TArray<FString>& NodeIds, bool bTransient)
{
    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const FString& NodeId : NodeIds)
    {
        ALoomaSyncedActor* Actor = FindSyncedActor(NodeId);
        if (!Actor)
        {
            continue;
        }
        TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), NodeId);
        // Relative, for an attached actor as much as a root: the wire's `t` is always
        // parent-local, and the hub folds it into the live document verbatim. The pose
        // read here must be the same quantity the diff in TickOutbound compared, or a
        // node attached to a non-node would be measured in one frame and reported in
        // another — hence WirePose on both sides rather than GetLocalTransform.
        Node->SetObjectField(TEXT("t"), LoomaUeToWire(WirePose(*Actor)));
        Nodes.Add(MakeShared<FJsonValueObject>(Node));
    }
    if (Nodes.Num() == 0)
    {
        return;
    }
    TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
    Msg->SetStringField(TEXT("type"), TEXT("transform"));
    Msg->SetBoolField(TEXT("transient"), bTransient);
    Msg->SetArrayField(TEXT("nodes"), Nodes);
    SendJson(Msg);
}

void ULoomaSceneSyncSubsystem::SendReparent(const TArray<FString>& NodeIds)
{
    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const FString& NodeId : NodeIds)
    {
        ALoomaSyncedActor* Actor = FindSyncedActor(NodeId);
        if (!Actor)
        {
            continue;
        }
        TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), NodeId);
        // Explicitly null rather than omitted: `parent` is a required field of the op —
        // "no parent" is a value here, not a field we had nothing to say about — and it
        // is the only way to tell the hub a node has left the hierarchy.
        if (Actor->ParentId.IsEmpty())
        {
            Node->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
        }
        else
        {
            Node->SetStringField(TEXT("parent"), Actor->ParentId);
        }
        // The node's pose *under its new parent*, which is the whole point of sending
        // one: it is what keeps the object still on screen across the move. We get it
        // for free — UE attached keeping the world pose, so the relative transform it
        // left behind is already the pose-preserving value the web works out by hand
        // (`localUnder` in frontend/src/commands.js).
        Node->SetObjectField(TEXT("t"), LoomaUeToWire(WirePose(*Actor)));
        Nodes.Add(MakeShared<FJsonValueObject>(Node));
    }
    if (Nodes.Num() == 0)
    {
        return;
    }
    // Fire and forget. The hub drops a move naming a parent it does not know (and one
    // that would close a loop, which the Outliner will not let you build) and
    // broadcasts nothing back — so we would hold an edge it does not have. Deliberately
    // not reconciled: tracking in-flight reparents to await an ack is a lot of
    // machinery for a case the editor makes hard to reach, and the next `scene`
    // message, which is the whole document, corrects us anyway.
    TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
    Msg->SetStringField(TEXT("type"), TEXT("reparent"));
    Msg->SetArrayField(TEXT("nodes"), Nodes);
    SendJson(Msg);
}

TStatId ULoomaSceneSyncSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULoomaSceneSyncSubsystem, STATGROUP_Tickables);
}

bool ULoomaSceneSyncSubsystem::IsTickable() const
{
    return GetGameInstance() != nullptr && GetGameInstance()->GetWorld() != nullptr;
}

// --- Local spawning (Unreal -> web) -------------------------------------------

ALoomaSyncedActor* ULoomaSceneSyncSubsystem::SpawnSyncedAsset(const FString& AssetId, const FString& Name,
    const FTransform& Transform, const FString& JobId)
{
    UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
    if (!World)
    {
        return nullptr;
    }

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ALoomaSyncedActor* Actor = World->SpawnActor<ALoomaSyncedActor>(Transform.GetLocation(), Transform.GetRotation().Rotator(), Params);
    if (!Actor)
    {
        return nullptr;
    }
    Actor->Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
    Actor->DisplayName = Name.IsEmpty() ? AssetId : Name;
    Actor->JobId = JobId;
    Actor->SetLocalTransform(Transform); // a root, so local == world
    Actor->OnDestroyed.AddDynamic(this, &ULoomaSceneSyncSubsystem::OnSyncedActorDestroyed);
#if WITH_EDITOR
    Actor->SetActorLabel(Actor->DisplayName);
#endif

    // The GLB is a component of this node now, not a property of the actor.
    FLoomaNodeComponents Components;
    Components.bHasModel = true;
    Components.Model.AssetId = AssetId;
    Components.Model.JobId = JobId;
    Actor->ApplyComponents(Components, MakeRenderContext(Components));

    FLoomaTrackedActor Entry;
    Entry.Actor = Actor;
    Entry.LastSent = Transform;
    Tracked.Add(Actor->Id, Entry);

    TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("id"), Actor->Id);
    // Explicitly null rather than omitted: `parent` is a required field, and a spawn
    // from Unreal lands at the root.
    Node->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
    Node->SetStringField(TEXT("name"), Actor->DisplayName);
    Node->SetObjectField(TEXT("t"), LoomaUeToWire(Transform));
    TArray<TSharedPtr<FJsonValue>> ComponentValues;
    // The url is for the web client's benefit only — it renders nothing without one.
    ComponentValues.Add(MakeShared<FJsonValueObject>(
        LoomaMakeModelComponent(AssetId, JobId, MakeWebAssetUrl(AssetId))));
    Node->SetArrayField(TEXT("components"), ComponentValues);

    TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
    Msg->SetStringField(TEXT("type"), TEXT("spawn"));
    TArray<TSharedPtr<FJsonValue>> Nodes;
    Nodes.Add(MakeShared<FJsonValueObject>(Node));
    Msg->SetArrayField(TEXT("nodes"), Nodes);
    SendJson(Msg);

    return Actor;
}

void ULoomaSceneSyncSubsystem::DespawnSyncedActor(ALoomaSyncedActor* Actor)
{
    if (Actor)
    {
        Actor->Destroy(); // OnSyncedActorDestroyed broadcasts the despawn
    }
}

// --- Presence: who else is in the room ----------------------------------------

void ULoomaSceneSyncSubsystem::HandleClients(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    if (!Msg->TryGetArrayField(TEXT("clients"), Entries) || !Entries)
    {
        return;
    }

    FString You;
    Msg->TryGetStringField(TEXT("you"), You);
    // Belt and braces, exactly as the web passes both `you` and its socket's own id.
    // `you` is the contract's answer and wins where the two differ, but a hub that
    // named someone else as us would otherwise be invisible — and its consequence is
    // the worst one this feature has: we would treat our own entry as a remote client
    // and paint our colour over our own selection. Cheap to notice, so notice it.
    if (!You.IsEmpty() && You != ClientId)
    {
        UE_LOG(LogLoomaSync, Warning,
            TEXT("Roster names us '%s' but our hello sent clientId '%s' — treating both as ours. ")
            TEXT("A hub that renamed our socket would make us draw our own colour on our own selection."),
            *You, *ClientId);
    }

    // Rebuilt wholesale, never merged. The roster is the whole room every time: a
    // client that has left is simply absent, and dropping it here is the only thing
    // that will ever clear its borders, because there is no teardown message.
    // Roster order is join order and is the claim tiebreak, so nothing below sorts.
    TArray<FLoomaClient> NextClients;
    NextClients.Reserve(Entries->Num());
    FLoomaClient NextSelf;
    FString NextOwnId = You;

    for (const TSharedPtr<FJsonValue>& Value : *Entries)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry)
        {
            continue;
        }
        FLoomaClient Client = LoomaParseClient(*Entry);
        if (Client.Id.IsEmpty())
        {
            // Nobody: an entry with no id cannot be attributed, so it cannot own a
            // border either. Verbose rather than a warning — this is a malformed hub,
            // not a state a running room reaches.
            UE_LOG(LogLoomaSync, Verbose, TEXT("Roster entry with no id, ignored"));
            continue;
        }
        // Ours goes aside rather than into the list. See GetClients() for why this one
        // line is the rule the whole feature rests on.
        if (Client.Id == You || Client.Id == ClientId)
        {
            NextSelf = MoveTemp(Client);
            // Where `you` was absent — an older hub, or a truncated message — our own
            // clientId is still a usable self name, so adopt it rather than leaving
            // GetOwnClientId() empty with a self entry sitting right there.
            if (NextOwnId.IsEmpty())
            {
                NextOwnId = NextSelf.Id;
            }
            continue;
        }
        NextClients.Add(MoveTemp(Client));
    }

    // --- The roster meets the ledger ----------------------------------------
    //
    // "Hydrate from the roster, then apply increments" (docs/scene-format.md). The
    // roster's job is to hydrate a client we are meeting for the first time — nothing
    // replays the `selection` messages we were not connected for — and NOT to restate
    // one we have been tracking incrementally since.
    //
    // So a client already in the room keeps the selection we hold for it, and only its
    // colour, role, name and kind are taken from the new roster. Two things go wrong
    // if the roster's copy is applied to a known client instead. It reorders: rebuilding
    // the ledger from roster order would re-seat every claim in JOIN order, so a node B
    // claimed before A would silently flip to A the next time an unrelated third party
    // joined — a border changing colour because somebody else walked in. And it can
    // lose: the hub awaits between roster recipients, so a roster generated before our
    // `selection` was recorded can be delivered after it, and treating that stale copy
    // as authoritative would retract a claim nothing will ever re-send.
    //
    // Membership and order are still wholesale, which is the step-1 rule intact: this
    // carries forward one field of an entry the roster still has to list at all.
    TSet<FString> PreviousIds;
    PreviousIds.Reserve(RemoteClients.Num());
    for (const FLoomaClient& Client : RemoteClients)
    {
        PreviousIds.Add(Client.Id);
    }
    for (FLoomaClient& Client : NextClients)
    {
        if (const FLoomaClient* Known = RemoteClients.FindByPredicate(
                [&Client](const FLoomaClient& Candidate) { return Candidate.Id == Client.Id; }))
        {
            Client.Selection = Known->Selection;
        }
    }

    const bool bSelfMoved = NextSelf != SelfClient || NextOwnId != OwnClientId;
    const bool bRoomMoved = NextClients != RemoteClients;
    SelfClient = MoveTemp(NextSelf);
    OwnClientId = MoveTemp(NextOwnId);

    // Rule 3, and it must run before the hydration below rather than after: a client
    // that left has no borders, which is the only way a disconnect clears them — there
    // is no teardown message — and freeing its slots first is what lets a newcomer
    // inherit a contested node in the same roster that reports both events.
    TSet<FString> NextIds;
    NextIds.Reserve(NextClients.Num());
    for (const FLoomaClient& Client : NextClients)
    {
        NextIds.Add(Client.Id);
    }
    for (const FString& OldId : PreviousIds)
    {
        if (!NextIds.Contains(OldId))
        {
            ReleaseAllClaims(OldId);
        }
    }

    RemoteClients = MoveTemp(NextClients);

    // Hydration proper, in roster order, which is join order — the tiebreak the
    // contract specifies for nodes that arrive already selected in a roster. Only
    // clients we had not met: everyone else's claims are already in the ledger, at the
    // positions their `selection` messages earned them.
    for (const FLoomaClient& Client : RemoteClients)
    {
        if (PreviousIds.Contains(Client.Id))
        {
            continue;
        }
        for (const FString& NodeId : Client.Selection)
        {
            ClaimNode(NodeId, Client.Id);
        }
    }

    // Only on an actual change — see FLoomaClientsEvent. The self half is folded into
    // the same event rather than given one of its own: a consumer showing "the colour
    // everyone else sees me in" has exactly one roster to read, and two events for one
    // message would only ever fire together.
    if (bRoomMoved || bSelfMoved)
    {
        MarkBordersDirty();
        OnClientsChanged.Broadcast(RemoteClients);
    }
}

void ULoomaSceneSyncSubsystem::HandleSelection(const TSharedPtr<FJsonObject>& Msg)
{
    FString SenderId;
    if (!Msg->TryGetStringField(TEXT("clientId"), SenderId) || SenderId.IsEmpty())
    {
        // Server-stamped and never optional on the inbound copy. Without it the claim
        // is unattributable — there is no colour to draw it in and no way to retract
        // it later — so there is nothing useful to do but drop the message.
        return;
    }
    // The hub echo-suppresses, so this should never be us. Checked anyway, for the
    // same reason step 1 keeps the self entry out of the remote list: our own colour
    // drawn on our own selection is the exact confusion per-client colours exist to
    // remove, and a one-line guard is cheaper than trusting a remote invariant.
    if (SenderId == ClientId || (!OwnClientId.IsEmpty() && SenderId == OwnClientId))
    {
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* IdValues = nullptr;
    if (!Msg->TryGetArrayField(TEXT("ids"), IdValues) || !IdValues)
    {
        // A *missing* `ids` is malformed; an EMPTY `ids` is a real message that clears
        // this client's borders everywhere. Reading the first as the second would let a
        // truncated message silently retract a live claim, so absence is refused here
        // rather than folded into the empty case.
        return;
    }
    TArray<FString> Ids;
    Ids.Reserve(IdValues->Num());
    for (const TSharedPtr<FJsonValue>& Value : *IdValues)
    {
        FString NodeId;
        if (Value.IsValid() && Value->TryGetString(NodeId) && !NodeId.IsEmpty())
        {
            // Kept whether or not we hold a node by this name. A selection legitimately
            // races the `spawn` that created its node, and filtering here would drop a
            // claim nothing ever re-sends; it simply becomes drawable if the node
            // arrives. Note this is the exact opposite of the OUTBOUND rule, where
            // Looma.Select refuses an id this client does not hold.
            Ids.Add(NodeId);
        }
    }

    FString ColorWire;
    Msg->TryGetStringField(TEXT("color"), ColorWire);
    const FString MessageHex = LoomaNormalizeClientColorHex(ColorWire);

    int32 Index = RemoteClients.IndexOfByPredicate(
        [&SenderId](const FLoomaClient& Candidate) { return Candidate.Id == SenderId; });
    if (Index == INDEX_NONE)
    {
        // A sender the roster has not introduced yet. Appended rather than ignored:
        // the roster and a `selection` are fanned out independently — the hub awaits
        // between roster recipients — so a selection really can land just before the
        // roster that names its sender, and dropping it would lose the claim for good.
        // The hub stamps `color` on this message precisely so the border can be drawn
        // anyway. Appended at the end, which is not its join position; the next roster
        // rebuilds membership and order and seats it correctly.
        FLoomaClient Provisional;
        Provisional.Id = SenderId;
        Provisional.Role = TEXT("unknown");
        // DisplayName left empty and Kind left Guest: neither field is on this message
        // — both are roster-only — and inventing either would put a name or an account
        // in the room that no other client is showing.
        Index = RemoteClients.Add(MoveTemp(Provisional));
    }

    FLoomaClient& Sender = RemoteClients[Index];
    // Colour precedence: this message, then whatever the last roster gave this client,
    // then the neutral fallback. The message's copy is the same value as the roster's,
    // so the first two can only differ when the hub has recoloured someone — and a
    // client we learned about from a `selection` alone has nothing but the message.
    if (!MessageHex.IsEmpty())
    {
        LoomaParseClientColor(MessageHex, Sender.ColorHex, Sender.Color);
    }
    else if (Sender.ColorHex.IsEmpty())
    {
        LoomaParseClientColor(FString(), Sender.ColorHex, Sender.Color);
    }

    if (Sender.Selection == Ids)
    {
        // Nothing moved. The ledger would be unchanged too — MoveClaims would compute
        // an empty diff — so returning here only skips an event, which is the rule
        // OnClientsChanged states.
        return;
    }
    // The whole set, replacing what it held; the wire never sends a delta. The ledger
    // gets the difference, which is why the old set has to be read before the new one
    // is stored.
    MoveClaims(SenderId, Sender.Selection, Ids);
    Sender.Selection = MoveTemp(Ids);

    MarkBordersDirty();
    OnClientsChanged.Broadcast(RemoteClients);
}

// --- The claim ledger ---------------------------------------------------------

void ULoomaSceneSyncSubsystem::ClaimNode(const FString& NodeId, const FString& ClaimantId)
{
    TArray<FString>& Claimants = Claims.FindOrAdd(NodeId);
    // AddUnique, spelled out: a claimant already on the list keeps the position it
    // has. Appending again would move it to the back of a queue it may be at the front
    // of, which is the one thing the tiebreak may never do.
    Claimants.AddUnique(ClaimantId);
}

void ULoomaSceneSyncSubsystem::ReleaseNode(const FString& NodeId, const FString& ClaimantId)
{
    TArray<FString>* Claimants = Claims.Find(NodeId);
    if (!Claimants)
    {
        return;
    }
    Claimants->Remove(ClaimantId);
    if (Claimants->Num() == 0)
    {
        // Dropped rather than left empty, so that "no entry" is the only way a node is
        // unclaimed. Two representations of the same state is how a lookup that checks
        // one of them starts drawing borders nobody holds.
        Claims.Remove(NodeId);
    }
}

void ULoomaSceneSyncSubsystem::ReleaseAllClaims(const FString& ClaimantId)
{
    // The one operation that scans, and the reason the ledger can afford to be keyed
    // by node alone — it runs once per roster, over the nodes a room has selected.
    for (auto It = Claims.CreateIterator(); It; ++It)
    {
        It.Value().Remove(ClaimantId);
        if (It.Value().Num() == 0)
        {
            It.RemoveCurrent();
        }
    }
}

void ULoomaSceneSyncSubsystem::MoveClaims(
    const FString& ClaimantId, const TArray<FString>& Held, const TArray<FString>& Next)
{
    const TSet<FString> StillHeld(Next);
    for (const FString& NodeId : Held)
    {
        if (!StillHeld.Contains(NodeId))
        {
            ReleaseNode(NodeId, ClaimantId);
        }
    }
    // ClaimNode is already idempotent, so the ids in both sets could simply be
    // re-claimed — but going through them anyway would make the no-op path depend on
    // AddUnique's silence for correctness rather than by construction. Diff both ways.
    const TSet<FString> WasHeld(Held);
    for (const FString& NodeId : Next)
    {
        if (!WasHeld.Contains(NodeId))
        {
            ClaimNode(NodeId, ClaimantId);
        }
    }
}

bool ULoomaSceneSyncSubsystem::GetNodeBorderOwner(const FString& NodeId, FLoomaClient& OutClient) const
{
    const TArray<FString>* Claimants = Claims.Find(NodeId);
    if (!Claimants || Claimants->Num() == 0)
    {
        return false;
    }
    // The head, and that is the whole rule. Derived here rather than read from a
    // maintained owner table: it is an array index, and a cached copy of a
    // one-element derivation is a thing that can disagree with its source.
    return GetClient((*Claimants)[0], OutClient);
}

TArray<FString> ULoomaSceneSyncSubsystem::GetNodeClaimants(const FString& NodeId) const
{
    if (const TArray<FString>* Claimants = Claims.Find(NodeId))
    {
        return *Claimants;
    }
    return TArray<FString>();
}

TArray<FString> ULoomaSceneSyncSubsystem::GetClientBorderNodes(const FString& InClientId) const
{
    FLoomaClient Client;
    if (!GetClient(InClientId, Client))
    {
        return TArray<FString>();
    }
    // The reverse index the ledger deliberately does not keep: that client's own
    // selection, filtered through one O(1) lookup per id. Order is the selection's,
    // so a renderer that groups by client gets a stable list across redraws.
    TArray<FString> Won;
    Won.Reserve(Client.Selection.Num());
    for (const FString& NodeId : Client.Selection)
    {
        const TArray<FString>* Claimants = Claims.Find(NodeId);
        if (Claimants && Claimants->Num() > 0 && (*Claimants)[0] == InClientId)
        {
            Won.Add(NodeId);
        }
    }
    return Won;
}

// --- Drawing the borders ------------------------------------------------------

TArray<FLoomaBorderGroup> ULoomaSceneSyncSubsystem::GetRemoteBorderGroups() const
{
    return BorderGroups;
}

TArray<FString> ULoomaSceneSyncSubsystem::GetUndrawnClients() const
{
    return UndrawnClientIds;
}

void ULoomaSceneSyncSubsystem::TickBorders()
{
    if (!bBordersDirty)
    {
        return;
    }
    bBordersDirty = false;
    RefreshRemoteBorders();
}

void ULoomaSceneSyncSubsystem::RefreshRemoteBorders()
{
    // --- Subtraction 1: our own selection ------------------------------------
    // On our screen ours always wins. A remote claim may never make us lose track of
    // what we hold, so these ids are removed from the remote borders entirely — both
    // the thick pass and the descendant hint — before anything is allocated.
    const TSet<FString> LocalIds(GetLocalSelectionIds());

    // --- Subtraction 2: a claimed node is never a descendant hint ------------
    // Seeded with EVERY claimed node, ours included, not merely the ones that end up
    // drawn: the same precedence a selection has over a child hint locally, so one
    // node keeps one border however many claimed subtrees it happens to sit under.
    TSet<FString> Taken;
    Taken.Reserve(Claims.Num());
    for (const TPair<FString, TArray<FString>>& Entry : Claims)
    {
        Taken.Add(Entry.Key);
    }

    // The parent -> children index, rebuilt here rather than maintained. It is derived
    // from ParentId, which UpsertNode and HandleReparent already keep true, and a
    // second maintained copy of the hierarchy is a thing that can disagree with the
    // attachment. O(nodes) against a recompute that only runs when something moved.
    TMap<FString, TArray<FString>> ChildIds;
    ChildIds.Reserve(Tracked.Num());
    for (const TPair<FString, FLoomaTrackedActor>& Entry : Tracked)
    {
        const ALoomaSyncedActor* Actor = Entry.Value.Actor.Get();
        if (Actor && !Actor->ParentId.IsEmpty())
        {
            ChildIds.FindOrAdd(Actor->ParentId).Add(Entry.Key);
        }
    }

    TArray<FLoomaBorderGroup> NextGroups;
    TArray<FString> NextUndrawn;
    int32 NextSlot = 1; // 0 is reserved for "no border" and is never allocated.

    // Roster order, which is join order: the same order the web's allocator spends its
    // passes in, so two clients looking at one room agree about who is drawn and who
    // was left out rather than each picking a different eight.
    for (const FLoomaClient& Client : RemoteClients)
    {
        FLoomaBorderGroup Group;
        Group.ClientId = Client.Id;
        Group.Color = Client.Color;
        for (const FString& NodeId : Client.Selection)
        {
            if (LocalIds.Contains(NodeId))
            {
                continue;
            }
            const TArray<FString>* Claimants = Claims.Find(NodeId);
            // The tiebreak, and the only place drawing consults it: the head, or
            // nothing. A node this client selected but did not claim first is somebody
            // else's border, and it is not drawn twice.
            if (Claimants && Claimants->Num() > 0 && (*Claimants)[0] == Client.Id)
            {
                Group.OwnNodeIds.Add(NodeId);
            }
        }
        if (Group.OwnNodeIds.Num() == 0)
        {
            // A client holding nothing spends no slot. That is what keeps the budget
            // spent on people who are actually working rather than on people present.
            continue;
        }
        if (NextSlot > LoomaRemoteBorderSlots)
        {
            NextUndrawn.Add(Client.Id);
            continue;
        }
        Group.Slot = NextSlot++;

        // The descendant hint: everything under what this client won. Breadth-first
        // from its own nodes, skipping ours, skipping anything already claimed
        // outright, and skipping what an earlier group already took — one node, one
        // border, and earlier means earlier in the roster.
        TArray<FString> Frontier = Group.OwnNodeIds;
        while (Frontier.Num() > 0)
        {
            const FString ParentId = Frontier.Pop(EAllowShrinking::No);
            const TArray<FString>* Kids = ChildIds.Find(ParentId);
            if (!Kids)
            {
                continue;
            }
            for (const FString& ChildId : *Kids)
            {
                if (LocalIds.Contains(ChildId) || Taken.Contains(ChildId))
                {
                    continue;
                }
                Taken.Add(ChildId);
                Group.ChildNodeIds.Add(ChildId);
                // Only a node we actually took is descended into. A skipped one is
                // somebody else's outright claim or our own selection, and both own
                // their subtree — walking through them would paint hints on children
                // that belong to whoever holds the node above them.
                Frontier.Add(ChildId);
            }
        }
        NextGroups.Add(MoveTemp(Group));
    }

    // --- Apply -----------------------------------------------------------------
    TSet<TWeakObjectPtr<UPrimitiveComponent>> Touched;
    Touched.Reserve(StencilComponents.Num());
    for (const FLoomaBorderGroup& Group : NextGroups)
    {
        for (const FString& NodeId : Group.OwnNodeIds)
        {
            ApplyStencilToNode(NodeId, LoomaBorderStencilValue(Group.Slot, /*bChild=*/false), Touched);
        }
        for (const FString& NodeId : Group.ChildNodeIds)
        {
            ApplyStencilToNode(NodeId, LoomaBorderStencilValue(Group.Slot, /*bChild=*/true), Touched);
        }
    }
    // Everything we had marked and no longer do, restored. This is the half that goes
    // wrong if it is left to a sweep: without the explicit set we would either have to
    // walk every actor in the world or leave a border on a node whose owner
    // disconnected, and the second is exactly the stale claim presence exists to
    // avoid. A component destroyed in the meantime resolves to null and is simply
    // dropped.
    for (const TWeakObjectPtr<UPrimitiveComponent>& Weak : StencilComponents)
    {
        if (Touched.Contains(Weak))
        {
            continue;
        }
        if (UPrimitiveComponent* Primitive = Weak.Get())
        {
            Primitive->SetCustomDepthStencilValue(0);
            Primitive->SetRenderCustomDepth(false);
        }
    }
    StencilComponents = MoveTemp(Touched);

    if (NextUndrawn != UndrawnClientIds && NextUndrawn.Num() > 0)
    {
        // Said out loud, once per change. A client that is present, working and
        // invisible is indistinguishable from an empty room, and a budget nobody is
        // told about is a bug report about borders that "sometimes do not work".
        UE_LOG(LogLoomaSync, Warning,
            TEXT("%d client(s) hold a border with no stencil slot left (budget is %d): %s. ")
            TEXT("Show Get Undrawn Clients in your UI — they are in the room and drawing nothing."),
            NextUndrawn.Num(), LoomaRemoteBorderSlots, *FString::Join(NextUndrawn, TEXT(", ")));
    }
    BorderGroups = MoveTemp(NextGroups);
    UndrawnClientIds = MoveTemp(NextUndrawn);

    // Checked here rather than at startup, and only once there is something to draw:
    // a project that never has a remote client in it does not need to hear about a
    // render setting it is not using, and a warning nobody can act on is noise that
    // teaches people to ignore the log.
    if (BorderGroups.Num() > 0)
    {
        static IConsoleVariable* CustomDepthCVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
        const int32 CustomDepth = CustomDepthCVar ? CustomDepthCVar->GetInt() : -1;
        // 3 is "Enabled with Stencil". At 0 or 1 the stencil buffer is not written at
        // all and NOTHING appears, however correct everything else is — the failure
        // that looks like a broken plugin and is a project setting. It cannot be set
        // from here: it is the host project's Project Settings > Rendering >
        // Postprocessing > Custom Depth-Stencil Pass, and writing to a host's render
        // config from a plugin would be a worse surprise than the warning.
        if (CustomDepth != 3 && !bWarnedCustomDepthOff)
        {
            bWarnedCustomDepthOff = true;
            UE_LOG(LogLoomaSync, Warning,
                TEXT("r.CustomDepth is %d, so remote selection borders will NOT render. Set Project ")
                TEXT("Settings > Rendering > Postprocessing > Custom Depth-Stencil Pass to ")
                TEXT("'Enabled with Stencil' (r.CustomDepth=3). Stencil values are being written; ")
                TEXT("nothing is reading them."),
                CustomDepth);
        }
        else if (CustomDepth == 3)
        {
            bWarnedCustomDepthOff = false;
        }
    }

    PublishBorderColors();
}

void ULoomaSceneSyncSubsystem::ApplyStencilToNode(
    const FString& NodeId, int32 StencilValue, TSet<TWeakObjectPtr<UPrimitiveComponent>>& OutTouched)
{
    const FLoomaTrackedActor* Entry = Tracked.Find(NodeId);
    ALoomaSyncedActor* Actor = Entry ? Entry->Actor.Get() : nullptr;
    if (!Actor)
    {
        // A claim on a node this client does not hold, which is a legitimate state and
        // not an error: a `selection` races the `spawn` that created its node, and the
        // claim stays in the ledger so the border appears if the node arrives. Nothing
        // to mark, so nothing happens.
        return;
    }
    // Every primitive on the actor, rather than ModelComponent and MeshComponent by
    // name. A node renders through whichever of them its components asked for, both
    // may be present at once, and reading the class rather than the two fields means a
    // third component type added later is outlined without this code being touched.
    //
    // Note what this does NOT reach: a `light` node and an empty group node have no
    // primitive at all, so their claim is held in the ledger and draws nothing. The
    // web solved that with a pick proxy (HAM-148); this plugin has none, and the
    // README says so rather than the behaviour being a surprise.
    //
    // `AActor::` is not optional here: ALoomaSyncedActor has its own GetComponents()
    // returning the node's *wire* components (FLoomaNodeComponents), which hides the
    // engine one entirely. Two meanings of "components" one call apart, and only the
    // qualification says which is meant.
    TInlineComponentArray<UPrimitiveComponent*> Primitives;
    Actor->AActor::GetComponents(Primitives);
    for (UPrimitiveComponent* Primitive : Primitives)
    {
        if (!Primitive)
        {
            continue;
        }
        Primitive->SetCustomDepthStencilValue(StencilValue);
        Primitive->SetRenderCustomDepth(true);
        OutTouched.Add(Primitive);
    }
}

void ULoomaSceneSyncSubsystem::PublishBorderColors()
{
    const FSoftObjectPath& Path = ULoomaSceneSyncSettings::Get().RemoteSelectionCollection;
    if (Path.IsNull())
    {
        // Not an error — a project may be driving its own material from
        // GetRemoteBorderGroups — but never silent either, because "no collection
        // configured" and "the feature is broken" produce the identical symptom of no
        // borders, and only one of them has an obvious fix.
        if (BorderGroups.Num() > 0 && !bWarnedNoBorderCollection)
        {
            bWarnedNoBorderCollection = true;
            UE_LOG(LogLoomaSync, Warning,
                TEXT("A remote client holds a border, but no Remote Selection Parameter Collection is ")
                TEXT("set (Project Settings > Plugins > Looma Scene Sync). Stencil values are being ")
                TEXT("written; no colours are being published. See the plugin README, 'Wiring the ")
                TEXT("outline' — or drive your own material from Get Remote Border Groups."));
        }
        return;
    }
    bWarnedNoBorderCollection = false;

    if (!ResolvedBorderCollection || ResolvedBorderCollection->GetPathName() != Path.ToString())
    {
        // Synchronous, and deliberately: this is one small uasset, it is loaded once
        // for the life of the game instance, and the first frame a border exists is
        // the wrong one to defer it to — an async load would leave the stencil written
        // and the colours absent for however long it took, which reads as the wrong
        // colours rather than as a load in progress.
        ResolvedBorderCollection = Cast<UMaterialParameterCollection>(Path.TryLoad());
        if (!ResolvedBorderCollection)
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Remote Selection Parameter Collection '%s' could not be loaded as a Material ")
                TEXT("Parameter Collection. Remote borders will have no colours."), *Path.ToString());
            return;
        }
    }

    UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
    if (!World)
    {
        return;
    }

    // Every slot written every time, including the empty ones. Writing only the
    // occupied slots would leave a departed client's colour sitting in the collection,
    // and a material that reads a slot the stencil no longer names would draw with it
    // — which is the "border in the wrong colour" failure, strictly worse than none.
    // Alpha is the occupancy flag, so a material can reject an unused slot without a
    // second parameter to keep in step.
    for (int32 Slot = 1; Slot <= LoomaRemoteBorderSlots; ++Slot)
    {
        const FLoomaBorderGroup* Group = BorderGroups.FindByPredicate(
            [Slot](const FLoomaBorderGroup& Candidate) { return Candidate.Slot == Slot; });
        FLinearColor Value = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        if (Group)
        {
            Value = Group->Color;
            Value.A = 1.0f;
        }
        UKismetMaterialLibrary::SetVectorParameterValue(World, ResolvedBorderCollection,
            FName(*FString::Printf(TEXT("LoomaClient%d"), Slot)), Value);
    }
    UKismetMaterialLibrary::SetScalarParameterValue(World, ResolvedBorderCollection,
        TEXT("LoomaClientCount"), static_cast<float>(BorderGroups.Num()));
}

void ULoomaSceneSyncSubsystem::ClearPresence()
{
    const bool bHadRoom = RemoteClients.Num() > 0 || !OwnClientId.IsEmpty() || !SelfClient.Id.IsEmpty();
    RemoteClients.Reset();
    SelfClient = FLoomaClient();
    OwnClientId.Reset();
    // The ledger goes with the room it describes. It is derived entirely from the
    // roster and the `selection` messages of the socket that just died, and nothing
    // will retract a claim made over a connection that no longer exists.
    Claims.Reset();
    if (bHadRoom)
    {
        // Dirty, not cleared inline: the recompute is the one place that knows which
        // primitives we marked, and it will now find nothing to draw and restore every
        // one of them. Clearing here as well would be a second implementation of the
        // same sweep, and the two would drift.
        MarkBordersDirty();
        OnClientsChanged.Broadcast(RemoteClients);
    }
}

TArray<FLoomaClient> ULoomaSceneSyncSubsystem::GetClients() const
{
    return RemoteClients;
}

FString ULoomaSceneSyncSubsystem::GetOwnClientId() const
{
    return OwnClientId;
}

// `InClientId` rather than the header's `ClientId`, which is the Blueprint pin name:
// the member of the same name is this socket's own id, and shadowing it here would be
// one typo away from answering "is this us" with the wrong variable.
bool ULoomaSceneSyncSubsystem::GetClient(const FString& InClientId, FLoomaClient& OutClient) const
{
    if (InClientId.IsEmpty())
    {
        return false;
    }
    // Ours first, and it is not in the array — the split is a storage decision, not an
    // "our entry is unreachable" one.
    if (!SelfClient.Id.IsEmpty() && SelfClient.Id == InClientId)
    {
        OutClient = SelfClient;
        return true;
    }
    if (const FLoomaClient* Found = RemoteClients.FindByPredicate(
            [&InClientId](const FLoomaClient& Client) { return Client.Id == InClientId; }))
    {
        OutClient = *Found;
        return true;
    }
    return false;
}

void ULoomaSceneSyncSubsystem::LogRoom() const
{
    // One log call per client rather than one joined block, so the engine's line
    // wrapping does not fold a roster of five into an unreadable paragraph.
    auto LogClient = [](const FLoomaClient& Client, bool bIsSelf) {
        UE_LOG(LogLoomaSync, Display, TEXT("  %s %s | %s | %s | %s | %s | selection: %s"),
            bIsSelf ? TEXT("*") : TEXT(" "),
            *Client.Id,
            Client.DisplayName.IsEmpty() ? TEXT("<no name>") : *Client.DisplayName,
            Client.Kind == ELoomaClientKind::User ? TEXT("user") : TEXT("guest"),
            *Client.Role,
            *Client.ColorHex,
            Client.Selection.Num() == 0 ? TEXT("<empty>") : *FString::Join(Client.Selection, TEXT(", ")));
    };

    if (OwnClientId.IsEmpty() && RemoteClients.Num() == 0)
    {
        // Distinguished, because "no roster yet" and "a room with nobody else in it"
        // look identical from an empty list and mean opposite things — the first is a
        // socket problem, the second is simply being alone.
        UE_LOG(LogLoomaSync, Display, TEXT("Looma room: no roster held (%s)"),
            IsSyncConnected() ? TEXT("connected, none received yet") : TEXT("not connected"));
        return;
    }

    UE_LOG(LogLoomaSync, Display, TEXT("Looma room: %d other client(s); our id is %s (* marks us)"),
        RemoteClients.Num(), OwnClientId.IsEmpty() ? TEXT("<unknown>") : *OwnClientId);
    // Ours first rather than in its roster position: the position is not kept, since
    // nothing but the remote list needs join order.
    if (!SelfClient.Id.IsEmpty())
    {
        LogClient(SelfClient, /*bIsSelf=*/true);
    }
    else if (!OwnClientId.IsEmpty())
    {
        UE_LOG(LogLoomaSync, Display,
            TEXT("  * %s | <not in the roster the hub sent us>"), *OwnClientId);
    }
    for (const FLoomaClient& Client : RemoteClients)
    {
        LogClient(Client, /*bIsSelf=*/false);
    }
}

void ULoomaSceneSyncSubsystem::LogClaims() const
{
    if (Claims.Num() == 0)
    {
        UE_LOG(LogLoomaSync, Display, TEXT("Looma claims: nobody in the room has anything selected"));
        return;
    }

    UE_LOG(LogLoomaSync, Display, TEXT("Looma claims: %d node(s) claimed (winner first, '<-' marks the border)"),
        Claims.Num());
    // Iteration order of a TMap is its internal one, so this listing is not stable
    // between calls. Deliberately not sorted: the order WITHIN each list is the
    // load-bearing thing and it is preserved, while sorting the nodes would suggest an
    // ordering between them that the ledger does not have.
    for (const TPair<FString, TArray<FString>>& Entry : Claims)
    {
        UE_LOG(LogLoomaSync, Display, TEXT("  %s <- %s"),
            *Entry.Key, *FString::Join(Entry.Value, TEXT(", then ")));
    }

    // The other direction, and the reason both are printed: what a client selected and
    // what it actually draws are different numbers the moment anyone is contested, and
    // a border that is simply missing looks identical to a client that is not there.
    UE_LOG(LogLoomaSync, Display, TEXT("Looma claims by client:"));
    for (const FLoomaClient& Client : RemoteClients)
    {
        const TArray<FString> Won = GetClientBorderNodes(Client.Id);
        UE_LOG(LogLoomaSync, Display, TEXT("    %s (%s) wins %d of %d selected%s%s"),
            *Client.Id,
            Client.DisplayName.IsEmpty() ? *Client.ColorHex : *Client.DisplayName,
            Won.Num(), Client.Selection.Num(),
            Won.Num() > 0 ? TEXT(": ") : TEXT(""),
            Won.Num() > 0 ? *FString::Join(Won, TEXT(", ")) : TEXT(""));
    }

    // What is actually being marked, which is a third number again: a client can win a
    // claim and still draw nothing, either because the budget ran out or because the
    // node it won has no primitive to outline (a light, or an empty group node).
    UE_LOG(LogLoomaSync, Display, TEXT("Looma borders: %d of %d stencil slot(s) in use%s%s"),
        BorderGroups.Num(), LoomaRemoteBorderSlots,
        UndrawnClientIds.Num() > 0 ? TEXT("; no slot left for: ") : TEXT(""),
        UndrawnClientIds.Num() > 0 ? *FString::Join(UndrawnClientIds, TEXT(", ")) : TEXT(""));
    for (const FLoomaBorderGroup& Group : BorderGroups)
    {
        UE_LOG(LogLoomaSync, Display, TEXT("    slot %d (%s) stencil %d/%d: %d node(s), %d descendant(s)"),
            Group.Slot, *Group.ClientId,
            LoomaBorderStencilValue(Group.Slot, /*bChild=*/false),
            LoomaBorderStencilValue(Group.Slot, /*bChild=*/true),
            Group.OwnNodeIds.Num(), Group.ChildNodeIds.Num());
    }
}

// --- Generation jobs ----------------------------------------------------------

void ULoomaSceneSyncSubsystem::HandleGeneration(const TSharedPtr<FJsonObject>& Msg)
{
    const TSharedPtr<FJsonObject>* JobField = nullptr;
    if (!Msg->TryGetObjectField(TEXT("job"), JobField) || !JobField)
    {
        return;
    }
    const FLoomaGenerationJob Job = LoomaParseGenerationJob(*JobField);
    if (Job.JobId.IsEmpty())
    {
        return;
    }
    ApplyJob(Job);
}

void ULoomaSceneSyncSubsystem::ApplyJob(const FLoomaGenerationJob& InJob)
{
    FLoomaGenerationJob Job = InJob;
    // The suggested spawn pose is set once, at submit, and never cleared — but the
    // backend announces the job before storing it, so early events arrive without
    // one. Learn it the first time we see it, then merge it into every later
    // snapshot so the cache, the hub-wide events and the handles all agree.
    if (Job.bHasSuggestedTransform)
    {
        SuggestedTransforms.Add(Job.JobId, Job.SuggestedTransform);
    }
    else if (const FTransform* Known = SuggestedTransforms.Find(Job.JobId))
    {
        Job.SuggestedTransform = *Known;
        Job.bHasSuggestedTransform = true;
    }

    Jobs.Add(Job.JobId, Job);
    OnGenerationJobUpdated.Broadcast(Job);
    switch (Job.State)
    {
    case ELoomaJobState::AwaitingImage:
        OnGenerationImagesReady.Broadcast(Job);
        break;
    case ELoomaJobState::Done:
        OnGenerationJobDone.Broadcast(Job);
        break;
    case ELoomaJobState::Failed:
        OnGenerationJobFailed.Broadcast(Job);
        break;
    default:
        break;
    }

    // Then the per-job listeners, if anyone asked for a handle on this job.
    if (const TObjectPtr<ULoomaGenerationHandle>* Found = JobHandles.Find(Job.JobId))
    {
        if (ULoomaGenerationHandle* Handle = Found->Get())
        {
            Handle->Apply(Job);
        }
    }
}

void ULoomaSceneSyncSubsystem::NoteSuggestedTransform(const FString& JobId, const FTransform& SuggestedTransform)
{
    if (JobId.IsEmpty())
    {
        return;
    }
    SuggestedTransforms.Add(JobId, SuggestedTransform);

    // Patch a snapshot that already landed without the pose, plus any handle
    // already handed out, so nothing has to wait for the next event.
    if (FLoomaGenerationJob* Cached = Jobs.Find(JobId))
    {
        Cached->SuggestedTransform = SuggestedTransform;
        Cached->bHasSuggestedTransform = true;
    }
    if (const TObjectPtr<ULoomaGenerationHandle>* Found = JobHandles.Find(JobId))
    {
        if (ULoomaGenerationHandle* Handle = Found->Get())
        {
            Handle->SeedTransform(SuggestedTransform);
        }
    }
}

ULoomaGenerationHandle* ULoomaSceneSyncSubsystem::GetGenerationHandle(const FString& JobId)
{
    if (JobId.IsEmpty())
    {
        return nullptr;
    }
    if (const TObjectPtr<ULoomaGenerationHandle>* Found = JobHandles.Find(JobId))
    {
        if (ULoomaGenerationHandle* Existing = Found->Get())
        {
            return Existing;
        }
    }

    ULoomaGenerationHandle* Handle = NewObject<ULoomaGenerationHandle>(this);
    Handle->JobId = JobId;
    JobHandles.Add(JobId, Handle);

    // If the job is already known, replay it — but next tick, so the caller has
    // this frame to bind its events. Submitting a new job hits neither branch:
    // there is nothing cached yet, and the first real event arrives over the WS.
    if (Jobs.Contains(JobId))
    {
        PendingHandleReplays.AddUnique(JobId);
    }
    return Handle;
}

void ULoomaSceneSyncSubsystem::FlushPendingHandleReplays()
{
    if (PendingHandleReplays.IsEmpty())
    {
        return;
    }
    TArray<FString> Pending = MoveTemp(PendingHandleReplays);
    PendingHandleReplays.Reset();
    for (const FString& JobId : Pending)
    {
        const TObjectPtr<ULoomaGenerationHandle>* Found = JobHandles.Find(JobId);
        ULoomaGenerationHandle* Handle = Found ? Found->Get() : nullptr;
        const FLoomaGenerationJob* Known = Jobs.Find(JobId);
        // Skip if a live event already reached the handle first — it carried at
        // least as fresh a snapshot as the cache, and re-applying would repeat
        // OnUpdated for no reason.
        if (Handle && Known && !Handle->bHasApplied)
        {
            Handle->Apply(*Known);
        }
    }
}

void ULoomaSceneSyncSubsystem::HydrateGenerationQueue()
{
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(GetRestBase() + TEXT("/generate"));
    Request->SetVerb(TEXT("GET"));
    ApplyAuthHeader(Request);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [this](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 300)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("GET /generate hydrate failed (%d)"),
                    Resp.IsValid() ? Resp->GetResponseCode() : 0);
                return;
            }
            TArray<TSharedPtr<FJsonValue>> Items;
            const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Resp->GetContentAsString());
            if (!FJsonSerializer::Deserialize(Reader, Items))
            {
                return;
            }
            int32 Count = 0;
            for (const TSharedPtr<FJsonValue>& V : Items)
            {
                const FLoomaGenerationJob Job = LoomaParseGenerationJob(V->AsObject());
                if (!Job.JobId.IsEmpty())
                {
                    ApplyJob(Job);
                    ++Count;
                }
            }
            UE_LOG(LogLoomaSync, Log, TEXT("Hydrated %d generation job(s)"), Count);
        });
    Request->ProcessRequest();
}

void ULoomaSceneSyncSubsystem::SendRest(const FString& Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body)
{
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(GetRestBase() + Path);
    Request->SetVerb(Verb);
    // After SetURL, as ApplyAuthHeader requires. Every fire-and-forget REST call the
    // subsystem makes goes through here, so this one line is most of "the whole client
    // speaks as the logged-in account".
    ApplyAuthHeader(Request);
    if (Body.IsValid())
    {
        Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
        FString BodyText;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&BodyText);
        FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
        Request->SetContentAsString(BodyText);
    }
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [Verb, Path](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() >= 300)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("REST %s %s failed (%d)"),
                    *Verb, *Path, Resp.IsValid() ? Resp->GetResponseCode() : 0);
            }
            // On success the resulting state arrives over the WS `generation` event.
        });
    Request->ProcessRequest();
}

bool ULoomaSceneSyncSubsystem::GetGenerationJob(const FString& JobId, FLoomaGenerationJob& OutJob) const
{
    if (const FLoomaGenerationJob* Found = Jobs.Find(JobId))
    {
        OutJob = *Found;
        return true;
    }
    return false;
}

TArray<FLoomaGenerationJob> ULoomaSceneSyncSubsystem::GetAllGenerationJobs() const
{
    TArray<FLoomaGenerationJob> Out;
    Jobs.GenerateValueArray(Out);
    return Out;
}

ALoomaSyncedActor* ULoomaSceneSyncSubsystem::FindSyncedActorByJobId(const FString& JobId) const
{
    if (JobId.IsEmpty())
    {
        return nullptr;
    }
    for (const TPair<FString, FLoomaTrackedActor>& Pair : Tracked)
    {
        if (ALoomaSyncedActor* Actor = Pair.Value.Actor.Get())
        {
            if (Actor->JobId == JobId)
            {
                return Actor;
            }
        }
    }
    return nullptr;
}

void ULoomaSceneSyncSubsystem::SelectImage(const FString& JobId, const FString& ImageId)
{
    if (JobId.IsEmpty() || ImageId.IsEmpty())
    {
        return;
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("image_id"), ImageId);
    SendRest(TEXT("POST"), FString::Printf(TEXT("/generate/%s/select"), *JobId), Body);
}

void ULoomaSceneSyncSubsystem::RegenerateImages(const FString& JobId, const FString& Prompt, int32 NImages)
{
    if (JobId.IsEmpty())
    {
        return;
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    if (!Prompt.IsEmpty())
    {
        Body->SetStringField(TEXT("prompt"), Prompt);
    }
    if (NImages > 0)
    {
        Body->SetNumberField(TEXT("n_images"), NImages);
    }
    SendRest(TEXT("POST"), FString::Printf(TEXT("/generate/%s/regenerate"), *JobId), Body);
}

void ULoomaSceneSyncSubsystem::CancelGeneration(const FString& JobId)
{
    if (JobId.IsEmpty())
    {
        return;
    }
    SendRest(TEXT("DELETE"), FString::Printf(TEXT("/generate/%s"), *JobId), nullptr);
}

FString ULoomaSceneSyncSubsystem::GetRestBase() const
{
    return NormalizeRestBase(ULoomaSceneSyncSettings::Get().BackendUrl);
}

FString ULoomaSceneSyncSubsystem::ResolveBackendUrl(const FString& PathOrUrl) const
{
    if (PathOrUrl.IsEmpty())
    {
        return FString();
    }
    if (PathOrUrl.StartsWith(TEXT("http://")) || PathOrUrl.StartsWith(TEXT("https://")))
    {
        return PathOrUrl; // already absolute
    }
    FString Path = PathOrUrl;
    if (!Path.StartsWith(TEXT("/")))
    {
        Path = TEXT("/") + Path;
    }
    // Drop the web proxy prefix: "/api/static/x.png" -> "/static/x.png".
    if (Path.StartsWith(TEXT("/api/")))
    {
        Path = Path.RightChop(4);
    }
    else if (Path == TEXT("/api"))
    {
        Path = TEXT("/");
    }
    return GetRestBase() + Path;
}
