#include "LoomaSceneSyncSubsystem.h"

#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IWebSocket.h"
#include "LoomaGenerationHandle.h"
#include "LoomaGenerationTypes.h"
#include "LoomaSyncedActor.h"
#include "LoomaWireConvert.h"
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
    JobHandles.Empty();
    PendingHandleReplays.Empty();
    SuggestedTransforms.Empty();
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
        OnSyncConnected.Broadcast();
        // The WS never replays generation events to late joiners — pull the
        // current queue over REST so cached jobs reflect all clients.
        HydrateGenerationQueue();
    });
    Socket->OnConnectionError().AddWeakLambda(this, [this](const FString& Error) {
        UE_LOG(LogLoomaSync, Warning, TEXT("Connection error: %s (retrying)"), *Error);
        ReconnectCooldown = ReconnectDelay;
        OnSyncDisconnected.Broadcast();
    });
    Socket->OnClosed().AddWeakLambda(this, [this](int32 Code, const FString& Reason, bool bWasClean) {
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
    else if (Type == TEXT("generation"))
    {
        HandleGeneration(Msg);
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
        Obj->TryGetObjectField(TEXT("t"), TField) ? LoomaWireToUe(*TField) : FTransform::Identity;

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
    // A spawn may carry the generation job that produced it (see SpawnSyncedAsset)
    // so a Blueprint orb can match this real model to its placeholder.
    Obj->TryGetStringField(TEXT("jobId"), Actor->JobId);
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
        Obj->SetObjectField(TEXT("t"), LoomaUeToWire(Actor->GetActorTransform()));
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
    Actor->SetActorTransform(Transform);
    Actor->Guid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
    Actor->AssetId = AssetId;
    Actor->DisplayName = Name.IsEmpty() ? AssetId : Name;
    Actor->JobId = JobId;
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
    Obj->SetObjectField(TEXT("t"), LoomaUeToWire(Transform));
    if (!JobId.IsEmpty())
    {
        // Tells peers which generation job this instance came from, so they can
        // match it to a placeholder of their own.
        Obj->SetStringField(TEXT("jobId"), JobId);
    }

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
    return FString::Printf(TEXT("http://%s"), *BackendHost);
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
