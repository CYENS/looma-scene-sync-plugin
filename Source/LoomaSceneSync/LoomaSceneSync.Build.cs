using UnrealBuildTool;

public class LoomaSceneSync : ModuleRules
{
    public LoomaSceneSync(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            // ULoomaSceneSyncSettings — Project Settings > Plugins > Looma Scene Sync.
            "DeveloperSettings",
            // Runtime GLB loading straight from the backend's /static URLs.
            "glTFRuntime",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "WebSockets",
            "Json",
            // Generation-job REST calls (submit/select/regenerate/cancel, queue
            // hydrate) and candidate/selected-image downloads.
            "HTTP",
            "JsonUtilities",
            // Decode downloaded PNGs into UTexture2D (FImageUtils / IImageWrapper).
            "ImageWrapper",
        });

        // The plugin's only editor dependency, and it must stay editor-only: UnrealEd
        // does not exist in a packaged build, so linking it unconditionally would make
        // the plugin unshippable. Everything behind it is guarded by WITH_EDITOR in the
        // source as well — the Build.cs block and the #if are two halves of one fact,
        // and a build with either one missing fails in a way that is confusing rather
        // than obvious.
        //
        // Used for exactly one thing: mirroring the editor's own actor selection into
        // the outbound `selection` message (USelection::SelectionChangedEvent, GEditor).
        // The explicit-API half of that feature has no editor dependency at all and is
        // the only half that works in a packaged VR build.
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}
