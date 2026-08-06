#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LoomaSceneComponents.h"
#include "LoomaSyncedActor.generated.h"

class UglTFRuntimeAsset;
class ULightComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/**
 * Everything outside the node itself that applying its components needs. Owned by
 * the subsystem, because it comes from the plugin's config and the backend host.
 */
struct FLoomaNodeRenderContext
{
    /** Absolute GLB url for the `model` component, rebuilt from its assetId. */
    FString ModelUrl;

    /** Multiplier on a light's wire intensity — see ApplyLight for the unit reasoning. */
    float LightIntensityScale = 1.0f;

    /**
     * Lift a GLB so its bounding-box floor sits on the node origin, which is what
     * the web client does (looma-xr-asset-demo/frontend/src/scene/pivot.js). Off
     * leaves the GLB centred on the origin, as this plugin did before format v3.
     */
    bool bBaseAlignModels = true;
};

/**
 * One synced scene node (scene format v3 — the normative contract is
 * looma-xr-asset-demo/docs/scene-format.md).
 *
 * **The root is a plain USceneComponent and nothing else.** A node is an object
 * with a transform; what it renders comes from its components, so geometry and
 * lights are children created on demand:
 *
 *   SceneRoot (USceneComponent)     the node's transform — always present
 *     ├─ ModelComponent             `model`    — a datalake GLB via glTFRuntime
 *     ├─ MeshComponent              `mesh`     — an engine primitive, `material` on it
 *     └─ LightComponent             `light`    — point / spot / directional
 *
 * That is what makes an empty object (no components at all), a light-only node, and
 * a node carrying both a mesh and a light equally expressible. An empty node is not
 * a degenerate case: its children's transforms are relative to it.
 *
 * The actor's **parent-local** transform mirrors the node's `t`; UE attachment
 * composes the world pose the same way three.js does. Remote transient transforms
 * ease toward a target in Tick (the same k=12/s as the web app's RemoteSmoother);
 * final transforms snap exactly.
 */
UCLASS()
class LOOMASCENESYNC_API ALoomaSyncedActor : public AActor
{
    GENERATED_BODY()

public:
    ALoomaSyncedActor();

    /**
     * Node identity — the same field, under the same name, in the saved document and
     * on the wire. (Was `Guid`, which no longer exists anywhere in the format.)
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString Id;

    /** Parent node id; empty for a root. Kept in step with the actor attachment. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString ParentId;

    /** Display name, as shown in the browser's outliner. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString DisplayName;

    /** Convenience mirror of the `model` component's assetId; empty if this node renders no GLB. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString AssetId;

    /** Generation job this instance came from, if it was spawned with one (else empty). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    FString JobId;

    /** The node's transform. Always present, even when nothing renders. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TObjectPtr<USceneComponent> SceneRoot;

    /** The `model` component's mesh — built from the GLB. Null unless the node has one. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TObjectPtr<UStaticMeshComponent> ModelComponent;

    /** The `mesh` component's primitive. Null unless the node has one. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TObjectPtr<UStaticMeshComponent> MeshComponent;

    /** The `light` component. Null unless the node has one; the class follows `lightType`. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TObjectPtr<ULightComponent> LightComponent;

    /** Dynamic instance carrying the `material` component's colour. Primitives only. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Looma")
    TObjectPtr<UMaterialInstanceDynamic> MeshMaterial;

    /** What this node currently renders. */
    const FLoomaNodeComponents& GetComponents() const { return Components; }

    /**
     * Create, update or destroy engine components so this actor matches `Next`.
     * Idempotent — structural ops are re-broadcast to their sender, and a component
     * edit arrives as a patch carrying the node's whole component array.
     */
    void ApplyComponents(const FLoomaNodeComponents& Next, const FLoomaNodeRenderContext& Context);

    /** Kick off the async GLB download + mesh build for the `model` component. */
    void LoadMeshFromUrl(const FString& Url);

    /** Parent-local pose — what the wire carries. Equals the world pose for a root. */
    FTransform GetLocalTransform() const;
    void SetLocalTransform(const FTransform& Local);

    /** Set where a remote peer wants this node, parent-local. bSnap = final (exact) pose. */
    void SetRemoteTarget(const FTransform& LocalTarget, bool bSnap);

    /** True while easing toward a remote target (outbound diff skips these). */
    bool HasRemoteTarget() const { return bHasRemoteTarget; }

    virtual void Tick(float DeltaSeconds) override;

private:
    void ApplyModel(const FLoomaNodeComponents& Next, const FLoomaNodeRenderContext& Context);
    void ApplyMesh(const FLoomaNodeComponents& Next);
    void ApplyMaterial(const FLoomaNodeComponents& Next);
    void ApplyLight(const FLoomaNodeComponents& Next, const FLoomaNodeRenderContext& Context);

    /** Create a mesh component in `Slot` on first use, attached to SceneRoot. */
    UStaticMeshComponent* EnsureMeshComponent(TObjectPtr<UStaticMeshComponent>& Slot);

    UFUNCTION()
    void OnGlbLoaded(UglTFRuntimeAsset* Asset);

    FLoomaNodeComponents Components;

    /** The url ModelComponent's mesh was built from, so an unchanged model isn't refetched. */
    FString LoadedModelUrl;

    /** Captured from the last ApplyComponents, for the async GLB completion. */
    bool bBaseAlignModel = true;

    FTransform RemoteTarget;
    bool bHasRemoteTarget = false;
};
