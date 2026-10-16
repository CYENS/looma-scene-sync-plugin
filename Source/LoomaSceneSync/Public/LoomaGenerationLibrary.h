#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LoomaGenerationTypes.h"
#include "LoomaGenerationLibrary.generated.h"

/**
 * Blueprint helpers for a generation job's meta, so a graph never has to write or
 * parse JSON by hand.
 *
 * A meta is your client's own context for a job. The backend keeps it exactly as
 * sent and hands it back on every update (FLoomaGenerationJob::MetaJson). Build one
 * with `Make Generation Meta`, feed it to `Submit Generation`'s MetaJson, and read a
 * value back with `Get Generation Meta String`. Both name a value by a dotted path,
 * so "unreal.environment" is the `environment` key inside the `unreal` group:
 *
 *     {"unreal": {"environment": "LVL_Demo"}}
 *
 * Group your keys under your client's name like that. Every client of every session
 * sees a job's meta, and the keys inside it are never re-cased, so the group is what
 * keeps two clients from colliding on a key. Never put a secret in it.
 */
UCLASS()
class LOOMASCENESYNC_API ULoomaGenerationLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Build a meta for `Submit Generation`'s MetaJson from string values keyed by
     * dotted path. {"unreal.environment": "LVL_Demo"} becomes
     * {"unreal":{"environment":"LVL_Demo"}}. Every value goes in as a JSON string.
     *
     * An empty map is no meta: true, with MetaJson empty. False, with MetaJson empty
     * and a warning in the log, when a path has an empty part ("unreal..environment")
     * or runs into another path in the map, as "unreal" and "unreal.environment" do:
     * one key cannot hold both a value and a group.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Generation")
    static bool MakeGenerationMeta(const TMap<FString, FString>& Values, FString& MetaJson);

    /**
     * Read one value of a job's meta by dotted path, such as "unreal.environment".
     *
     * A string comes back as it is; a number or a boolean as its JSON text ("3",
     * "0.5", "true"); a group or a list as compact JSON. False, with Value empty,
     * when the job has no meta, the path is not in it, or the path names a null.
     *
     * The backend reports a job, and can report its first progress, before it has
     * stored the meta, so on a job's earliest events a false can mean "not known
     * yet". Once the job is Done it always means the job has none.
     */
    UFUNCTION(BlueprintPure, Category = "Looma|Generation")
    static bool GetGenerationMetaString(const FLoomaGenerationJob& Job, const FString& Path, FString& Value);

    /**
     * What `Get Generation Meta String` does, on MetaJson text rather than a job: the
     * job's node and ALoomaSyncedActor's both read through this. Empty text is no meta.
     */
    static bool FindGenerationMetaString(const FString& MetaJson, const FString& Path, FString& Value);
};
