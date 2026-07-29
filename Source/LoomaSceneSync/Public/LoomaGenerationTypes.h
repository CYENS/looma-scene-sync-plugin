#pragma once

#include "CoreMinimal.h"
#include "LoomaGenerationTypes.generated.h"

class FJsonObject;

/**
 * Lifecycle of a text->3D generation job, mirroring the backend vocabulary
 * (backend/app/generator.py JobState). Terminal states: Done/Failed/Cancelled.
 */
UENUM(BlueprintType)
enum class ELoomaJobState : uint8
{
    Unknown        UMETA(DisplayName = "Unknown"),
    Queued         UMETA(DisplayName = "Queued"),
    Running        UMETA(DisplayName = "Running"),
    AwaitingImage  UMETA(DisplayName = "Awaiting Image"),
    Done           UMETA(DisplayName = "Done"),
    Failed         UMETA(DisplayName = "Failed"),
    Cancelled      UMETA(DisplayName = "Cancelled"),
};

/**
 * Which stage a Queued job is waiting on.
 *
 * A job passes through Queued twice: once before its candidate images are
 * rendered, and again after a candidate is picked, because the backend's
 * `select()` returns the job to `queued` rather than straight to `running`
 * (backend/app/generator.py). Carried by ULoomaGenerationHandle::OnQueueUpdate so
 * a Blueprint can tell the two apart without inspecting the job itself.
 */
UENUM(BlueprintType)
enum class ELoomaQueuePhase : uint8
{
    /** Waiting for the GPU to render candidate images. */
    Images UMETA(DisplayName = "Images"),

    /** A candidate was picked; waiting for the GPU to reconstruct the 3D asset. */
    Asset  UMETA(DisplayName = "Asset"),
};

/** One candidate reference image produced for a job (before 3D reconstruction). */
USTRUCT(BlueprintType)
struct FLoomaGeneratedImage
{
    GENERATED_BODY()

    /** Stable image id (e.g. "img0"); pass to SelectImage to pick this one. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString Id;

    /** Backend-relative or absolute URL of the PNG. Feed to DownloadImageAsTexture. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString Url;

    /** Diffusion seed used for this candidate. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    int32 Seed = 0;
};

/**
 * A generation job as seen by a client. Populated from the WS `generation`
 * event (camelCase) and the `GET /generate` hydrate (snake_case) — both parse
 * into this one struct via LoomaParseGenerationJob.
 */
USTRUCT(BlueprintType)
struct FLoomaGenerationJob
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString JobId;

    /** The user's original prompt text. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString Prompt;

    /** The improved/composed prompt, if the backend exposes one (empty until ready). */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString EnhancedPrompt;

    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    ELoomaJobState State = ELoomaJobState::Unknown;

    /** 0..1 progress of the current stage. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    float Progress = 0.0f;

    /** 1-based position while Queued; 0 otherwise. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    int32 QueuePosition = 0;

    /** Datalake asset id of the finished model (e.g. "gen-xxxx"); empty until Done. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString AssetId;

    /** URL of the finished GLB; empty until Done. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString AssetUrl;

    /** Candidate reference images (present once State == AwaitingImage). */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    TArray<FLoomaGeneratedImage> Images;

    /** URL of the selected candidate image (the "portal" thumbnail), if any. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString SelectedImageUrl;

    /**
     * Where the submitter suggests an instance of the finished model be placed,
     * already converted to UE space. Advisory metadata carried on the job: the
     * backend never acts on it, and any client may honour it, ignore it, or place
     * the model somewhere else entirely. Set with `Submit Generation`'s
     * bSuggestTransform; spawn it yourself with `Spawn Synced Asset`.
     *
     * Set once, when the job is submitted, and never cleared. Readable at every
     * update once known: the subsystem merges it into snapshots that arrive
     * without it (the backend announces a job before it has stored the pose).
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FTransform SuggestedTransform;

    /** True once a suggested pose is known for this job. See SuggestedTransform. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    bool bHasSuggestedTransform = false;

    /** Human-readable failure reason (set when State == Failed). */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString Error;

    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString CreatedAt;

    UPROPERTY(BlueprintReadOnly, Category = "Looma|Generation")
    FString UpdatedAt;
};

/** Map a backend state string ("queued"..."cancelled") to the enum. */
LOOMASCENESYNC_API ELoomaJobState LoomaJobStateFromString(const FString& State);

/**
 * Parse a job object from either transport. Accepts both camelCase (WS: jobId,
 * assetUrl, imageUrl, queuePosition, enhancedPrompt, createdAt) and snake_case
 * (REST: job_id, asset_url, image_url, queue_position, enhanced_prompt,
 * created_at). Returns a job with an empty JobId if the object is invalid.
 */
LOOMASCENESYNC_API FLoomaGenerationJob LoomaParseGenerationJob(const TSharedPtr<FJsonObject>& JobObj);
