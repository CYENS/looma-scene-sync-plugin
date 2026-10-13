#pragma once

#include "CoreMinimal.h"

/**
 * GLBs kept on disk between sessions, so a model is downloaded once.
 *
 * Measured reason: a scene of 36 models was 184 MB and took 141 s to arrive through
 * a tunnel doing about 1.3 MB/s, and it took that long again on every launch,
 * because nothing was kept. Building the 36 meshes was under six seconds of it.
 *
 * It is a cache of what the server said, never a second source of truth. Every model
 * is still asked for; the request carries the validators the server gave last time
 * (`ETag`, `Last-Modified`) and the server answers 304 when the file has not changed,
 * which measured 0.2 s through the same tunnel. So a GLB replaced on the backend under
 * the same id (an /admin re-ingest, a texture re-encode) is picked up on the next
 * load, and an entry stored without a validator would be unusable, so none is stored.
 *
 * One entry is two files named by a hash of the url: `<hash>.glb`, the bytes exactly
 * as downloaded, and `<hash>.meta`, written last, which is what makes the entry exist.
 * The meta records the byte count, so a GLB cut short by a crash is not served.
 *
 * Game thread only. Two viewers on one PC may share the directory: every write goes
 * to its own temporary file and is moved into place.
 */
class FLoomaGlbCache
{
public:
    /** `MaxBytes` of 0 or less means no limit. */
    FLoomaGlbCache(FString InDirectory, int64 InMaxBytes);

    /**
     * The validators to send with a request for `Url`. False when there is no usable
     * entry, in which case the request must be unconditional.
     */
    bool FindValidators(const FString& Url, FString& OutETag, FString& OutLastModified) const;

    /** The cached bytes for `Url`. Marks the entry as used, for Prune. */
    bool Load(const FString& Url, TArray<uint8>& OutBytes) const;

    /** Keep `Bytes` as `Url`'s entry. Does nothing when the server sent no validator. */
    void Store(const FString& Url, const TArray<uint8>& Bytes, const FString& ETag, const FString& LastModified);

    void Remove(const FString& Url) const;

    /** Delete the least recently used entries until the cache fits `MaxBytes`. */
    void Prune();

    /** Delete everything. Returns the bytes freed. */
    int64 Clear();

    /** Bytes of GLB on disk right now. Walks the directory. */
    int64 GetSizeOnDisk() const;

    const FString& GetDirectory() const { return Directory; }

private:
    struct FMeta
    {
        FString ETag;
        FString LastModified;
        int64 Size = 0;
    };
    FString PathFor(const FString& Url, const TCHAR* Extension) const;
    bool ReadMeta(const FString& Url, FMeta& Out) const;

    FString Directory;
    int64 MaxBytes = 0;
    /** Bytes stored since the last Prune, so Store does not walk the directory each time. */
    int64 StoredSincePrune = 0;
    /** What Prune last found on disk; negative before the first one. */
    int64 SizeAtLastPrune = -1;
};
