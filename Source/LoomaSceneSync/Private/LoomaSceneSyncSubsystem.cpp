#include "LoomaSceneSyncSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IWebSocket.h"
#include "LoomaGenerationHandle.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSceneComponents.h"
#include "LoomaSceneSyncLog.h"
#include "LoomaSceneSyncSettings.h"
#include "LoomaSyncedActor.h"
#include "LoomaWireConvert.h"
#include "Misc/Guid.h"
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
} // namespace

// --- Lifecycle ---------------------------------------------------------------

void ULoomaSceneSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
    FModuleManager::Get().LoadModuleChecked(TEXT("WebSockets"));
    SettingsChangedHandle = ULoomaSceneSyncSettings::OnSettingsChanged().AddUObject(
        this, &ULoomaSceneSyncSubsystem::OnSettingsChanged);
    Connect();
}

void ULoomaSceneSyncSubsystem::Deinitialize()
{
    ULoomaSceneSyncSettings::OnSettingsChanged().Remove(SettingsChangedHandle);
    SettingsChangedHandle.Reset();
    CloseSocket();
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
    Socket = FWebSocketsModule::Get().CreateWebSocket(SocketUrl, TEXT(""));
    bConnecting = true;

    Socket->OnConnected().AddWeakLambda(this, [this]() {
        bConnecting = false;
        UE_LOG(LogLoomaSync, Log, TEXT("Connected to %s"), *SocketUrl);
        TSharedRef<FJsonObject> Hello = MakeShared<FJsonObject>();
        Hello->SetStringField(TEXT("type"), TEXT("hello"));
        Hello->SetStringField(TEXT("clientId"), ClientId);
        Hello->SetStringField(TEXT("role"), TEXT("unreal"));
        SendJson(Hello);
        OnSyncConnected.Broadcast();
        // The WS never replays generation events to late joiners — pull the
        // current queue over REST so cached jobs reflect all clients.
        HydrateGenerationQueue();
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
        Reconnect();
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
    return FString::Printf(TEXT("%s | hub %s | rest %s | %d node(s), scene %s | %d job(s)"),
        *State,
        *HubUrl,
        *RestBase,
        Tracked.Num(),
        ActiveSceneId.IsEmpty() ? TEXT("<unsaved>") : *ActiveSceneId,
        Jobs.Num());
}

void ULoomaSceneSyncSubsystem::LogConnectionStatus()
{
    UE_LOG(LogLoomaSync, Display, TEXT("Looma Scene Sync: %s"), *GetConnectionStatusText());

    // A socket can be down for three different reasons — nothing listening, the wrong
    // host, or the right host with the wrong path prefix. One REST call tells them
    // apart, but only if we check *what* answered: a reverse proxy that serves the web
    // app at the root answers /health with 200 and an index.html, which reads as
    // healthy while the hub URL points at nothing that speaks WebSocket.
    const FString RestBase = GetRestBase();
    const FString HealthUrl = RestBase + TEXT("/health");
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(HealthUrl);
    Request->SetVerb(TEXT("GET"));
    Request->SetTimeout(5.0f);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [HealthUrl, RestBase](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk) {
            if (!bOk || !Resp.IsValid())
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("Backend %s unreachable — is the backend running, ")
                                              TEXT("and is the Backend URL right?"), *HealthUrl);
                return;
            }
            const int32 Code = Resp->GetResponseCode();
            const FString Body = Resp->GetContentAsString();
            const FString Excerpt = Body.Left(120).Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" "));
            if (Code >= 300)
            {
                UE_LOG(LogLoomaSync, Warning, TEXT("Backend %s answered %d: %s"), *HealthUrl, Code, *Excerpt);
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
                UE_LOG(LogLoomaSync, Display, TEXT("Backend %s OK (%d): status %s, %d asset(s)"), *HealthUrl, Code,
                    *Status, Assets);
                return;
            }
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Backend %s answered %d but not with the backend's /health JSON — something else is ")
                TEXT("serving that address (a web app, usually). If the API sits behind a path prefix, ")
                TEXT("the Backend URL must include it, e.g. %s/api. Got: %s"),
                *HealthUrl, Code, *RestBase, *Excerpt);
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
        if (Actor->HasRemoteTarget())
        {
            continue; // remote-driven right now — never echo it back
        }

        // Parent-local, like the wire: a child dragged inside its parent must report
        // its own motion, and a child carried *by* its parent must report none.
        const FTransform Current = Actor->GetLocalTransform();
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
        // parent-local, and the hub folds it into the live document verbatim.
        Node->SetObjectField(TEXT("t"), LoomaUeToWire(Actor->GetLocalTransform()));
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
