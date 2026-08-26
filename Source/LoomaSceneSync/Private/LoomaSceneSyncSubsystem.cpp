#include "LoomaSceneSyncSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IWebSocket.h"
#include "LoomaAuthTypes.h"
#include "LoomaGenerationHandle.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaSyncedActor.h"
#include "LoomaWireConvert.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
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
}

void ULoomaSceneSyncSubsystem::Deinitialize()
{
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
}

void ULoomaSceneSyncSubsystem::Connect()
{
    CloseSocket();
    SocketUrl = GetSceneSyncUrl();
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
        // Belt and braces, both documented: the hub's precedence is bearer header,
        // then session cookie, then `hello.token` (docs/scene-format.md), and all three
        // resolve to the same identity. The header wins when it survives the trip; the
        // hello field is what covers a proxy that strips Authorization on upgrade,
        // which this deployment — behind a Cloudflare tunnel — is exactly the shape of.
        // Sending both costs one field and removes a class of "works locally" bug.
        ApplyAuthToken(Hello);
        SendJson(Hello);
        OnSyncConnected.Broadcast();
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
        OnSyncDisconnected.Broadcast();
    });
    Socket->OnClosed().AddWeakLambda(this, [this](int32 Code, const FString& Reason, bool bWasClean) {
        bConnecting = false;
        UE_LOG(LogLoomaSync, Warning, TEXT("Socket closed (%d %s), retrying"), Code, *Reason);
        ReconnectCooldown = ReconnectDelay;
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
    // Only the backend address needs a new socket, and only when it actually moved —
    // every other knob is read where it is used.
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
    }
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
    if (IsSyncConnected())
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
    return FString::Printf(TEXT("%s | hub %s | rest %s | auth %s | %d node(s), scene %s | %d job(s)"),
        *State,
        *HubUrl,
        *RestBase,
        AuthStateText(AuthState),
        Tracked.Num(),
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
        return;
    }
    // `sceneId` is null for an unsaved working scene, and TryGet leaves the old value
    // in place on a null — so clear it first.
    ActiveSceneId.Reset();
    Msg->TryGetStringField(TEXT("sceneId"), ActiveSceneId);

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
