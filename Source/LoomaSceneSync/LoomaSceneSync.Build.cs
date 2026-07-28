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
            // Runtime GLB loading straight from the backend's /static URLs.
            "glTFRuntime",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "WebSockets",
            "Json",
        });
    }
}
