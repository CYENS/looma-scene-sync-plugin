using UnrealBuildTool;

/**
 * The optional UI half of the plugin (HAM-182): a login widget for an auth-enabled
 * backend, built on the `Looma|Auth` surface `LoomaSceneSync` exposes.
 *
 * Separate from the core module so the Slate dependency is contained. `LoomaSceneSync`
 * is a protocol and runtime module — a headless or dedicated-server build has no
 * business linking Slate — and a second module is what makes the widget genuinely
 * optional rather than nominally so: disable this one and nothing of the core changes.
 *
 * Dependencies flow ONE WAY. This module depends on LoomaSceneSync; LoomaSceneSync must
 * never gain a line pointing back. If that ever seems necessary, the split is wrong.
 *
 * Pure C++ Slate, no UMG and no plugin content — see the module header for why.
 */
public class LoomaSceneSyncUI : ModuleRules
{
    public LoomaSceneSyncUI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Private by default; a dependency is promoted to Public only when a *public*
        // header of this module includes one of its headers.
        //
        // `Public/LoomaLoginUI.h` is now such a header: it is a UCLASS deriving from
        // UBlueprintFunctionLibrary, so it includes Kismet/BlueprintFunctionLibrary.h
        // (Engine) and its own .generated.h (CoreUObject). Anyone including it needs
        // both include paths, so both are public — applying the rule above, not
        // relaxing it.
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            // UObject/UCLASS machinery, reached through LoomaLoginUI.generated.h.
            "CoreUObject",
            // UBlueprintFunctionLibrary in the public header; UGameViewportClient and
            // UWorld in the implementation.
            "Engine",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // The widget itself: SCompoundWidget and the editable-text/button set.
            // Deliberately private — a consumer of this module gets no transitive Slate
            // include path, which is the containment the two-module split is for.
            "Slate",
            "SlateCore",
            // The whole point of the module: ULoomaSceneSyncSubsystem's auth state,
            // identity and ULoomaLoginAction, plus LogLoomaSync so the plugin keeps one
            // log category. Never the other way round.
            "LoomaSceneSync",
        });
    }
}
