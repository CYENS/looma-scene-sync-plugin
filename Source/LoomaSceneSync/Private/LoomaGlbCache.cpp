#include "LoomaGlbCache.h"

#include "HAL/FileManager.h"
#include "LoomaSceneSyncLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
const TCHAR* GlbExtension = TEXT(".glb");
const TCHAR* MetaExtension = TEXT(".meta");

/** Write beside the target and move into place, so a reader never sees half a file. */
template <typename WriteFn>
bool WriteAtomically(const FString& Path, WriteFn&& Write)
{
    const FString Partial = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".partial");
    if (!Write(Partial))
    {
        IFileManager::Get().Delete(*Partial, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
        return false;
    }
    if (!IFileManager::Get().Move(*Path, *Partial, /*Replace=*/true, /*EvenIfReadOnly=*/true))
    {
        IFileManager::Get().Delete(*Partial, false, true, true);
        return false;
    }
    return true;
}
}

FLoomaGlbCache::FLoomaGlbCache(FString InDirectory, int64 InMaxBytes)
    : Directory(MoveTemp(InDirectory))
    , MaxBytes(InMaxBytes)
{
}

FString FLoomaGlbCache::PathFor(const FString& Url, const TCHAR* Extension) const
{
    return Directory / (FMD5::HashAnsiString(*Url) + Extension);
}

bool FLoomaGlbCache::ReadMeta(const FString& Url, FMeta& Out) const
{
    // Three lines: byte count, ETag, Last-Modified. A fourth, the url, is for whoever
    // opens the folder; nothing reads it back.
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *PathFor(Url, MetaExtension)) || Lines.Num() < 3)
    {
        return false;
    }
    Out.Size = FCString::Atoi64(*Lines[0]);
    Out.ETag = Lines[1];
    Out.LastModified = Lines[2];
    return Out.Size > 0 && (!Out.ETag.IsEmpty() || !Out.LastModified.IsEmpty());
}

bool FLoomaGlbCache::FindValidators(const FString& Url, FString& OutETag, FString& OutLastModified) const
{
    FMeta Meta;
    if (!ReadMeta(Url, Meta) || IFileManager::Get().FileSize(*PathFor(Url, GlbExtension)) != Meta.Size)
    {
        return false;
    }
    OutETag = Meta.ETag;
    OutLastModified = Meta.LastModified;
    return true;
}

bool FLoomaGlbCache::Load(const FString& Url, TArray<uint8>& OutBytes) const
{
    FMeta Meta;
    const FString GlbPath = PathFor(Url, GlbExtension);
    if (!ReadMeta(Url, Meta) || !FFileHelper::LoadFileToArray(OutBytes, *GlbPath, FILEREAD_Silent)
        || OutBytes.Num() != Meta.Size)
    {
        OutBytes.Reset();
        return false;
    }
    // Used now: Prune deletes the entries whose GLB was touched longest ago.
    IFileManager::Get().SetTimeStamp(*GlbPath, FDateTime::UtcNow());
    return true;
}

void FLoomaGlbCache::Store(const FString& Url, const TArray<uint8>& Bytes, const FString& ETag,
    const FString& LastModified)
{
    if (Bytes.IsEmpty() || (ETag.IsEmpty() && LastModified.IsEmpty()))
    {
        return;
    }
    // Meta out first, so the old meta can never describe the new bytes.
    const FString MetaPath = PathFor(Url, MetaExtension);
    IFileManager::Get().Delete(*MetaPath, false, true, true);
    const bool bStored = WriteAtomically(PathFor(Url, GlbExtension), [&Bytes](const FString& Partial) {
        return FFileHelper::SaveArrayToFile(Bytes, *Partial);
    }) && WriteAtomically(MetaPath, [&](const FString& Partial) {
        const FString Meta = FString::Printf(TEXT("%lld\n%s\n%s\n%s\n"),
            static_cast<int64>(Bytes.Num()), *ETag, *LastModified, *Url);
        return FFileHelper::SaveStringToFile(Meta, *Partial, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    });
    if (!bStored)
    {
        // A full or read-only disk costs the next launch a download, nothing more.
        UE_LOG(LogLoomaSync, Warning, TEXT("Could not cache %s under %s"), *Url, *Directory);
        return;
    }
    StoredSincePrune += Bytes.Num();
    if (MaxBytes > 0 && (SizeAtLastPrune < 0 || SizeAtLastPrune + StoredSincePrune > MaxBytes))
    {
        Prune();
    }
}

void FLoomaGlbCache::Remove(const FString& Url) const
{
    IFileManager::Get().Delete(*PathFor(Url, MetaExtension), false, true, true);
    IFileManager::Get().Delete(*PathFor(Url, GlbExtension), false, true, true);
}

void FLoomaGlbCache::Prune()
{
    struct FEntry
    {
        FString GlbPath;
        FDateTime Used;
        int64 Size = 0;
    };
    TArray<FEntry> Entries;
    int64 Total = 0;
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *(Directory / TEXT("*")), /*Files=*/true, /*Directories=*/false);
    for (const FString& Name : Names)
    {
        const FString Path = Directory / Name;
        if (Name.EndsWith(TEXT(".partial")))
        {
            // Left by a crash mid-write. A viewer writing right now holds its own for
            // well under a minute.
            if (IFileManager::Get().GetFileAgeSeconds(*Path) > 600.0)
            {
                IFileManager::Get().Delete(*Path, false, true, true);
            }
            continue;
        }
        if (!Name.EndsWith(GlbExtension))
        {
            continue;
        }
        const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
        Entries.Add({Path, Stat.ModificationTime, Stat.FileSize});
        Total += Stat.FileSize;
    }
    if (MaxBytes > 0 && Total > MaxBytes)
    {
        Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Used < B.Used; });
        int32 Deleted = 0;
        for (const FEntry& Entry : Entries)
        {
            if (Total <= MaxBytes)
            {
                break;
            }
            IFileManager::Get().Delete(*FPaths::ChangeExtension(Entry.GlbPath, MetaExtension), false, true, true);
            IFileManager::Get().Delete(*Entry.GlbPath, false, true, true);
            Total -= Entry.Size;
            ++Deleted;
        }
        UE_LOG(LogLoomaSync, Log, TEXT("Model cache over its %lld MB limit: dropped the %d least recently used"),
            MaxBytes / (1024 * 1024), Deleted);
    }
    SizeAtLastPrune = Total;
    StoredSincePrune = 0;
}

int64 FLoomaGlbCache::Clear()
{
    const int64 Freed = GetSizeOnDisk();
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *(Directory / TEXT("*")), true, false);
    for (const FString& Name : Names)
    {
        if (Name.EndsWith(GlbExtension) || Name.EndsWith(MetaExtension) || Name.EndsWith(TEXT(".partial")))
        {
            IFileManager::Get().Delete(*(Directory / Name), false, true, true);
        }
    }
    SizeAtLastPrune = 0;
    StoredSincePrune = 0;
    return Freed;
}

int64 FLoomaGlbCache::GetSizeOnDisk() const
{
    int64 Total = 0;
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *(Directory / (FString(TEXT("*")) + GlbExtension)), true, false);
    for (const FString& Name : Names)
    {
        Total += FMath::Max<int64>(0, IFileManager::Get().FileSize(*(Directory / Name)));
    }
    return Total;
}
