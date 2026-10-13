#include "LoomaGlbCache.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
/** A cache in a directory of its own, deleted with the fixture. */
struct FLoomaGlbCacheTestFixture
{
    FString Directory = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectIntermediateDir() / TEXT("LoomaGlbCacheTests") / FGuid::NewGuid().ToString());

    ~FLoomaGlbCacheTestFixture() { IFileManager::Get().DeleteDirectory(*Directory, false, true); }

    static TArray<uint8> Bytes(int32 Count, uint8 Fill)
    {
        TArray<uint8> Out;
        Out.Init(Fill, Count);
        return Out;
    }

    TArray<FString> Files(const TCHAR* Extension) const
    {
        TArray<FString> Names;
        IFileManager::Get().FindFiles(Names, *(Directory / (FString(TEXT("*")) + Extension)), true, false);
        return Names;
    }
};

const TCHAR* Chair = TEXT("https://example.test/api/static/chair.glb");
const TCHAR* Lamp = TEXT("https://example.test/api/static/lamp.glb");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGlbCacheRoundTripTest, "Looma.GlbCache.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLoomaGlbCacheRoundTripTest::RunTest(const FString&)
{
    FLoomaGlbCacheTestFixture Fixture;
    FLoomaGlbCache Cache(Fixture.Directory, 0);
    FString ETag, LastModified;
    TArray<uint8> Loaded;

    TestFalse(TEXT("an empty cache has no validators"), Cache.FindValidators(Chair, ETag, LastModified));
    TestFalse(TEXT("an empty cache loads nothing"), Cache.Load(Chair, Loaded));

    Cache.Store(Chair, FLoomaGlbCacheTestFixture::Bytes(1000, 7), TEXT("\"abc\""), TEXT("Mon, 28 Sep 2026 12:17:40 GMT"));

    TestTrue(TEXT("a stored model has validators"), Cache.FindValidators(Chair, ETag, LastModified));
    TestEqual(TEXT("the ETag comes back as the server sent it, quotes and all"), ETag, FString(TEXT("\"abc\"")));
    TestEqual(TEXT("so does Last-Modified"), LastModified, FString(TEXT("Mon, 28 Sep 2026 12:17:40 GMT")));
    TestTrue(TEXT("a stored model loads"), Cache.Load(Chair, Loaded));
    TestEqual(TEXT("with every byte"), Loaded, FLoomaGlbCacheTestFixture::Bytes(1000, 7));
    TestFalse(TEXT("another url is another entry"), Cache.FindValidators(Lamp, ETag, LastModified));

    // A second cache on the same directory is the next session.
    FLoomaGlbCache NextSession(Fixture.Directory, 0);
    TestTrue(TEXT("the entry outlives the object that stored it"), NextSession.Load(Chair, Loaded));

    Cache.Store(Chair, FLoomaGlbCacheTestFixture::Bytes(500, 9), TEXT("\"def\""), FString());
    TestTrue(TEXT("a newer download replaces the entry"), Cache.FindValidators(Chair, ETag, LastModified));
    TestEqual(TEXT("with its own ETag"), ETag, FString(TEXT("\"def\"")));
    TestTrue(TEXT("and loads"), Cache.Load(Chair, Loaded));
    TestEqual(TEXT("as the newer bytes"), Loaded, FLoomaGlbCacheTestFixture::Bytes(500, 9));

    Cache.Remove(Chair);
    TestFalse(TEXT("a removed entry is gone"), Cache.FindValidators(Chair, ETag, LastModified));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGlbCacheUnusableEntryTest, "Looma.GlbCache.UnusableEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLoomaGlbCacheUnusableEntryTest::RunTest(const FString&)
{
    FLoomaGlbCacheTestFixture Fixture;
    FLoomaGlbCache Cache(Fixture.Directory, 0);
    FString ETag, LastModified;
    TArray<uint8> Loaded;

    // No validator, no way to ask the server whether the copy is still right.
    Cache.Store(Chair, FLoomaGlbCacheTestFixture::Bytes(1000, 7), FString(), FString());
    TestFalse(TEXT("a response without validators is not kept"), Cache.FindValidators(Chair, ETag, LastModified));
    TestEqual(TEXT("and leaves no file"), Fixture.Files(TEXT(".glb")).Num(), 0);

    // A GLB cut short, as a crash mid-write would have left one before writes were
    // atomic, or as a hand-edited folder might.
    Cache.Store(Lamp, FLoomaGlbCacheTestFixture::Bytes(1000, 7), TEXT("\"abc\""), FString());
    const TArray<FString> Glbs = Fixture.Files(TEXT(".glb"));
    if (TestEqual(TEXT("one GLB on disk"), Glbs.Num(), 1))
    {
        FFileHelper::SaveArrayToFile(FLoomaGlbCacheTestFixture::Bytes(10, 7), *(Fixture.Directory / Glbs[0]));
        TestFalse(TEXT("a truncated GLB offers no validators"), Cache.FindValidators(Lamp, ETag, LastModified));
        TestFalse(TEXT("and is not served"), Cache.Load(Lamp, Loaded));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaGlbCachePruneTest, "Looma.GlbCache.Prune",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLoomaGlbCachePruneTest::RunTest(const FString&)
{
    FLoomaGlbCacheTestFixture Fixture;
    FString ETag, LastModified;
    TArray<uint8> Loaded;
    const FString Urls[] = {TEXT("https://example.test/a.glb"), TEXT("https://example.test/b.glb"),
        TEXT("https://example.test/c.glb")};
    {
        FLoomaGlbCache Unlimited(Fixture.Directory, 0);
        for (const FString& Url : Urls)
        {
            Unlimited.Store(Url, FLoomaGlbCacheTestFixture::Bytes(1000, 1), TEXT("\"x\""), FString());
        }
        TestEqual(TEXT("three models on disk"), Unlimited.GetSizeOnDisk(), static_cast<int64>(3000));
    }
    // Age all three by hand, then use two: a Load is what marks an entry as used.
    for (const FString& Name : Fixture.Files(TEXT(".glb")))
    {
        IFileManager::Get().SetTimeStamp(*(Fixture.Directory / Name), FDateTime::UtcNow() - FTimespan::FromHours(3.0));
    }
    FLoomaGlbCache Reader(Fixture.Directory, 0);
    TestTrue(TEXT("a loads"), Reader.Load(Urls[0], Loaded));
    TestTrue(TEXT("b loads"), Reader.Load(Urls[1], Loaded));

    FLoomaGlbCache Limited(Fixture.Directory, 2500);
    Limited.Prune();

    TestEqual(TEXT("pruned down to the limit"), Limited.GetSizeOnDisk(), static_cast<int64>(2000));
    TestTrue(TEXT("a, just used, is kept"), Limited.FindValidators(Urls[0], ETag, LastModified));
    TestTrue(TEXT("b, just used, is kept"), Limited.FindValidators(Urls[1], ETag, LastModified));
    TestFalse(TEXT("c, not used since it was stored, is the one dropped"),
        Limited.FindValidators(Urls[2], ETag, LastModified));
    TestEqual(TEXT("its meta went with it"), Fixture.Files(TEXT(".meta")).Num(), 2);

    TestEqual(TEXT("Clear reports what it freed"), Limited.Clear(), static_cast<int64>(2000));
    TestEqual(TEXT("and leaves nothing"), Limited.GetSizeOnDisk(), static_cast<int64>(0));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
