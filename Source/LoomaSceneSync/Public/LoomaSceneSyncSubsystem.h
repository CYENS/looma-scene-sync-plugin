#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "LoomaSceneSyncSubsystem.generated.h"

class ALoomaSyncedActor;
class IWebSocket;
class FJsonObject;

/** Per-actor bookkeeping for the outbound motion diff. */
struct FLoomaTrackedActor
{
    TWeakObjectPtr<ALoomaSyncedActor> Actor;
    FTransform LastSent;
    int32 StillFrames = 0;
    bool bMoving = false;
};

/**
 * Client of the LoomaXR scene-sync hub (backend /ws/scene).
 *
 * Inbound: hello -> snapshot -> spawn/despawn/transform messages become
 * ALoomaSyuncedActor instances whose meshes load at runtime from
 * http://<BackendHost>/static/<assetId>.glb (glTFRuntime).
 *
 * Outbound: every tick, each tracked actor's transform is diffed against a
 * last-sent cache — any motion (editor/PIE gizmo, physics, sequencer, code)
 * streams as transient transforms at <=30 Hz, with one final commit after the
 * actor comes to rest. Works in packaged builds (no editor delegates).
 *
 * Wire convention: right-handed, Y-up, meters, quaternion [x,y,z,w]
 * (three.js/glTF native). Conversion to UE (left-handed, Z-up, cm) is
 * (x,y,z) -> (-z,x,y) x100, matching glTFRuntime's DEFAULT SceneBasis
 * (FglTFRuntimeConfig::GetMatrix) so meshes and transforms agree.
 */
UCLASS(Config = Game)
class LOOMASCENESYNC_API ULoomaSceneSyncSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    // --- UGameInstanceSubsystem ---
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // --- FTickableGameObject ---
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override;

    /** Spawn a datalake asset here and on every connected peer (the web app). */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    ALoomaSyncedActor* SpawnSyncedAsset(const FString& AssetId, const FString& Name, const FTransform& Transform);

    /** Remove a synced actor here and on every connected peer. */
    UFUNCTION(BlueprintCallable, Category = "Looma")
    void DespawnSyncedActor(ALoomaSyncedActor* Actor);

    UFUNCTION(BlueprintPure, Category = "Looma")
    bool IsSyncConnected() const;

    /**
     * host:port of the FastAPI backend. Override in Config/DefaultGame.ini:
     *   [/Script/LoomaSceneSync.LoomaSceneSyncSubsystem]
     *   BackendHost=192.168.1.10:8000
     * Prefer an explicit IP over "localhost": UE's WebSocket client may
     * resolve localhost to IPv6 (::1) while uvicorn listens on IPv4 only.
     */
    UPROPERTY(Config)
    // FString BackendHost = TEXT("127.0.0.1:8000");
    // FString BackendHost = TEXT("teacher-monster-hear-wing.trycloudflare.com/api");
    // FString BackendHost = TEXT("closed-benefit-seed-jacket.trycloudflare.com/api");
    FString BackendHost = TEXT("translations-discovery-betty-vehicle.trycloudflare.com/api");

private:
    void Connect();
    void SendJson(const TSharedRef<FJsonObject>& Msg);

    // Inbound
    void OnRawMessage(const FString& Text);
    void HandleSnapshot(const TSharedPtr<FJsonObject>& Msg);
    void HandleSpawn(const TSharedPtr<FJsonObject>& Msg);
    void HandleDespawn(const TSharedPtr<FJsonObject>& Msg);
    void HandleTransform(const TSharedPtr<FJsonObject>& Msg);
    /** Spawn-or-update from one wire object (used by snapshot and spawn). */
    void UpsertRemoteObject(const TSharedPtr<FJsonObject>& Obj);

    UFUNCTION()
    void OnSyncedActorDestroyed(AActor* DestroyedActor);

    // Outbound motion diff
    void TickOutbound(float DeltaTime);
    void SendTransforms(const TArray<FString>& Guids, bool bTransient);

    TSharedPtr<IWebSocket> Socket;
    FString ClientId;
    TMap<FString, FLoomaTrackedActor> Tracked; // guid -> actor + last-sent cache
    bool bApplyingRemote = false;              // suppress outbound while applying inbound
    float ReconnectCooldown = 0.0f;
    float SinceLastTransientSend = 1.0f;
};
