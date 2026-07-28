#include "LoomaSceneSyncSubsystem.h"

#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "IWebSocket.h"
#include "LoomaSyncedActor.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebSocketsModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaSync, Log, All);

namespace
{
constexpr float ReconnectDelay = 2.0f;     // seconds between connection attempts
constexpr float TransientInterval = 0.033f; // ~30 Hz live streaming
constexpr int32 StillFramesForFinal = 10;   // rest frames before the final commit

// --- Wire <-> UE conversion --------------------------------------------------
// Wire: right-handed, Y-up, meters, quat [x,y,z,w]. UE: left-handed, Z-up, cm.
//
// Must match glTFRuntime's DEFAULT SceneBasis (glTFRuntimeParser.h,
// FglTFRuntimeConfig::GetMatrix), which maps glTF -> UE as
//   (x, y, z) -> (-z, x, y)      [glTF -Z forward -> UE +X forward, Y-up -> Z-up]
// so mesh geometry and our actor transforms agree. The map flips handedness
// (det = -1), so a rotation conjugates as: vector part through the axis map,
// then negated; w unchanged:  q_UE = (qz, -qx, -qy, qw).

FTransform WireToUe(const TSharedPtr<FJsonObject>& T)
{
    auto Num = [](const TArray<TSharedPtr<FJsonValue>>& A, int32 I, double Fallback) {
        return A.IsValidIndex(I) ? A[I]->AsNumber() : Fallback;
    };
    const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Q = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* S = nullptr;
    T->TryGetArrayField(TEXT("p"), P);
    T->TryGetArrayField(TEXT("q"), Q);
    T->TryGetArrayField(TEXT("s"), S);

    FVector Location = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
    if (P)
    {
        Location = FVector(-Num(*P, 2, 0) * 100.0, Num(*P, 0, 0) * 100.0, Num(*P, 1, 0) * 100.0);
    }
    if (Q)
    {
        Rotation = FQuat(Num(*Q, 2, 0), -Num(*Q, 0, 0), -Num(*Q, 1, 0), Num(*Q, 3, 1));
        Rotation.Normalize();
    }
    if (S)
    {
        Scale = FVector(Num(*S, 2, 1), Num(*S, 0, 1), Num(*S, 1, 1));
    }
    return FTransform(Rotation, Location, Scale);
}

TSharedRef<FJsonObject> UeToWire(const FTransform& T)
{
    const FVector L = T.GetLocation();
    const FQuat Q = T.GetRotation();
    const FVector S = T.GetScale3D();

    auto Arr = [](std::initializer_list<double> Values) {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (double V : Values)
        {
            Out.Add(MakeShared<FJsonValueNumber>(V));
        }
        return Out;
    };

    // Inverse of the map above: UE (X, Y, Z) -> wire (Y, Z, -X).
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetArrayField(TEXT("p"), Arr({ L.Y / 100.0, L.Z / 100.0, -L.X / 100.0 }));
    Obj->SetArrayField(TEXT("q"), Arr({ -Q.Y, -Q.Z, Q.X, Q.W }));
    Obj->SetArrayField(TEXT("s"), Arr({ S.Y, S.Z, S.X }));
    return Obj;
}
} // namespace

// --- Lifecycle ---------------------------------------------------------------

void ULoomaSceneSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
    FModuleManager::Get().LoadModuleChecked(TEXT("WebSockets"));
    Connect();
}

void ULoomaSceneSyncSubsystem::Deinitialize()
{
    if (Socket.IsValid())
    {
        Socket->OnConnected().Clear();
        Socket->OnConnectionError().Clear();
        Socket->OnClosed().Clear();
        Socket->OnMessage().Clear();
        Socket->Close();
        Socket.Reset();
    }
    Tracked.Empty();
    Super::Deinitialize();
}

void ULoomaSceneSyncSubsystem::Connect()
{
    const FString Url = FString::Printf(TEXT("ws://%s/ws/scene"), *BackendHost);
    Socket = FWebSocketsModule::Get().CreateWebSocket(Url, TEXT(""));

    Socket->OnConnected().AddWeakLambda(this, [this]() {
        UE_LOG(LogLoomaSync, Log, TEXT("Connected to %s"), *BackendHost);
        TSharedRef<FJsonObject> Hello = MakeShared<FJsonObject>();
        Hello->SetStringField(TEXT("type"), TEXT("hello"));
        Hello->SetStringField(TEXT("clientId"), ClientId);
        Hello->SetStringField(TEXT("role"), TEXT("unreal"));
        SendJson(Hello);
    });
    Socket->OnConnectionError().AddWeakLambda(this, [this](const FString& Error) {
        UE_LOG(LogLoomaSync, Warning, TEXT("Connection error: %s (retrying)"), *Error);
        ReconnectCooldown = ReconnectDelay;
    });
    Socket->OnClosed().AddWeakLambda(this, [this](int32 Code, const FString& Reason, bool bWasClean) {
        UE_LOG(LogLoomaSync, Warning, TEXT("Socket closed (%d %s), retrying"), Code, *Reason);
        ReconnectCooldown = ReconnectDelay;
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
    if (Type == TEXT("snapshot"))
    {
        HandleSnapshot(Msg);
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
    bApplyingRemote = false;
}

void ULoomaSceneSyncSubsystem::HandleSnapshot(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    if (!Msg->TryGetArrayField(TEXT("objects"), Objects))
    {
        return;
    }
    UE_LOG(LogLoomaSync, Log, TEXT("Snapshot: %d object(s)"), Objects->Num());
    for (const TSharedPtr<FJsonValue>& V : *Objects)
    {
        UpsertRemoteObject(V->AsObject());
    }
}

void ULoomaSceneSyncSubsystem::HandleSpawn(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    if (!Msg->TryGetArrayField(TEXT("objects"), Objects))
    {
        return;
    }
    for (const TSharedPtr<FJsonValue>& V : *Objects)
    {
        UpsertRemoteObject(V->AsObject());
    }
}

void ULoomaSceneSyncSubsystem::UpsertRemoteObject(const TSharedPtr<FJsonObject>& Obj)
{
    if (!Obj.IsValid())
    {
        return;
    }
    const FString Guid = Obj->GetStringField(TEXT("guid"));
    if (Guid.IsEmpty())
    {
        return;
    }

    const TSharedPtr<FJsonObject>* TField = nullptr;
    const FTransform Transform =
        Obj->TryGetObjectField(TEXT("t"), TField) ? WireToUe(*TField) : FTransform::Identity;

    if (FLoomaTrackedActor* Existing = Tracked.Find(Guid))
    {
        if (ALoomaSyncedActor* Actor = Existing->Actor.Get())
        {
            Actor->SetRemoteTarget(Transform, /*bSnap=*/true);
            Existing->LastSent = Transform;
            return;
        }
        Tracked.Remove(Guid);
    }

    UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
    if (!World)
    {
        return;
    }

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ALoomaSyncedActor* Actor = World->SpawnActor<ALoomaSyncedActor>(Transform.GetLocation(), Transform.GetRotation().Rotator(), Params);
    if (!Actor)
    {
        return;
    }
    Actor->SetActorTransform(Transform); // includes scale
    Actor->Guid = Guid;
    Actor->AssetId = Obj->GetStringField(TEXT("assetId"));
    Actor->DisplayName = Obj->GetStringField(TEXT("name"));
#if WITH_EDITOR
    Actor->SetActorLabel(Actor->DisplayName.IsEmpty() ? Guid : Actor->DisplayName);
#endif
    Actor->OnDestroyed.AddDynamic(this, &ULoomaSceneSyncSubsystem::OnSyncedActorDestroyed);
    Actor->LoadMeshFromUrl(FString::Printf(TEXT("http://%s/static/%s.glb"), *BackendHost, *Actor->AssetId));

    UE_LOG(LogLoomaSync, Log, TEXT("Spawned '%s' (%s) at %s rot %s scale %s"),
        *Actor->DisplayName, *Guid,
        *Transform.GetLocation().ToString(),
        *Transform.GetRotation().Rotator().ToString(),
        *Transform.GetScale3D().ToString());

    FLoomaTrackedActor Entry;
    Entry.Actor = Actor;
    Entry.LastSent = Transform;
    Tracked.Add(Guid, Entry);
}

void ULoomaSceneSyncSubsystem::HandleDespawn(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Guids = nullptr;
    if (!Msg->TryGetArrayField(TEXT("guids"), Guids))
    {
        return;
    }
    for (const TSharedPtr<FJsonValue>& V : *Guids)
    {
        FLoomaTrackedActor Entry;
        if (Tracked.RemoveAndCopyValue(V->AsString(), Entry))
        {
            if (ALoomaSyncedActor* Actor = Entry.Actor.Get())
            {
                Actor->Destroy(); // bApplyingRemote suppresses the despawn echo
            }
        }
    }
}

void ULoomaSceneSyncSubsystem::HandleTransform(const TSharedPtr<FJsonObject>& Msg)
{
    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    if (!Msg->TryGetArrayField(TEXT("objects"), Objects))
    {
        return;
    }
    bool bTransient = false;
    Msg->TryGetBoolField(TEXT("transient"), bTransient);

    for (const TSharedPtr<FJsonValue>& V : *Objects)
    {
        const TSharedPtr<FJsonObject> Obj = V->AsObject();
        if (!Obj.IsValid())
        {
            continue;
        }
        FLoomaTrackedActor* Entry = Tracked.Find(Obj->GetStringField(TEXT("guid")));
        const TSharedPtr<FJsonObject>* TField = nullptr;
        if (!Entry || !Obj->TryGetObjectField(TEXT("t"), TField))
        {
            continue;
        }
        const FTransform Target = WireToUe(*TField);
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

void ULoomaSceneSyncSubsystem::OnSyncedActorDestroyed(AActor* DestroyedActor)
{
    ALoomaSyncedActor* Actor = Cast<ALoomaSyncedActor>(DestroyedActor);
    if (!Actor || Actor->Guid.IsEmpty())
    {
        return;
    }
    const bool bWasTracked = Tracked.Remove(Actor->Guid) > 0;
    if (bWasTracked && !bApplyingRemote)
    {
        TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
        Msg->SetStringField(TEXT("type"), TEXT("despawn"));
        TArray<TSharedPtr<FJsonValue>> Guids;
        Guids.Add(MakeShared<FJsonValueString>(Actor->Guid));
        Msg->SetArrayField(TEXT("guids"), Guids);
        SendJson(Msg);
    }
}

// --- Outbound (subsystem tick: reconnect + motion diff) -----------------------

void ULoomaSceneSyncSubsystem::Tick(float DeltaTime)
{
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

        const FTransform Current = Actor->GetActorTransform();
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

void ULoomaSceneSyncSubsystem::SendTransforms(const TArray<FString>& Guids, bool bTransient)
{
    TArray<TSharedPtr<FJsonValue>> Objects;
    for (const FString& Guid : Guids)
    {
        const FLoomaTrackedActor* Entry = Tracked.Find(Guid);
        ALoomaSyncedActor* Actor = Entry ? Entry->Actor.Get() : nullptr;
        if (!Actor)
        {
            continue;
        }
        TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("guid"), Guid);
        Obj->SetObjectField(TEXT("t"), UeToWire(Actor->GetActorTransform()));
        Objects.Add(MakeShared<FJsonValueObject>(Obj));
    }
    if (Objects.Num() == 0)
    {
        return;
    }
    TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
    Msg->SetStringField(TEXT("type"), TEXT("transform"));
    Msg->SetBoolField(TEXT("transient"), bTransient);
    Msg->SetArrayField(TEXT("objects"), Objects);
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

ALoomaSyncedActor* ULoomaSceneSyncSubsystem::SpawnSyncedAsset(const FString& AssetId, const FString& Name, const FTransform& Transform)
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
    Actor->SetActorTransform(Transform);
    Actor->Guid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
    Actor->AssetId = AssetId;
    Actor->DisplayName = Name.IsEmpty() ? AssetId : Name;
    Actor->OnDestroyed.AddDynamic(this, &ULoomaSceneSyncSubsystem::OnSyncedActorDestroyed);
    Actor->LoadMeshFromUrl(FString::Printf(TEXT("http://%s/static/%s.glb"), *BackendHost, *AssetId));

    FLoomaTrackedActor Entry;
    Entry.Actor = Actor;
    Entry.LastSent = Transform;
    Tracked.Add(Actor->Guid, Entry);

    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("guid"), Actor->Guid);
    Obj->SetStringField(TEXT("assetId"), AssetId);
    Obj->SetStringField(TEXT("name"), Actor->DisplayName);
    Obj->SetObjectField(TEXT("t"), UeToWire(Transform));

    TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
    Msg->SetStringField(TEXT("type"), TEXT("spawn"));
    TArray<TSharedPtr<FJsonValue>> Objects;
    Objects.Add(MakeShared<FJsonValueObject>(Obj));
    Msg->SetArrayField(TEXT("objects"), Objects);
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
