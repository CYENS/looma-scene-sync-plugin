#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * The optional UI module: a bare-bones login widget for an auth-enabled backend
 * (HAM-182), built entirely on the `Looma|Auth` Blueprint surface HAM-181 shipped.
 *
 * ## Why this is a second module and not part of LoomaSceneSync
 *
 * So the Slate dependency is contained. The core module is a protocol and runtime
 * module; a headless or dedicated-server build has no business linking Slate. Two
 * modules is also what makes the widget genuinely optional rather than nominally so —
 * turn this one off in the .uplugin and the core compiles and behaves exactly as it
 * did, with no `#if` anywhere to get wrong.
 *
 * ## Pure C++ Slate, not a UUserWidget plus a WBP
 *
 * `LoomaSceneSync.uplugin` sets `"CanContainContent": false`, and this module keeps it
 * that way. The alternative — a `UUserWidget` base in C++ with a Blueprint widget in
 * plugin content — is friendlier to iterate on in-editor, and was weighed and rejected:
 *
 *   - It would make the plugin content-bearing, which is a change to what the plugin
 *     *is*. Today it drops into a project as code, with nothing to cook.
 *   - A WBP drifts from the C++ base it is parented to. Re-parenting, stale bindings and
 *     merge conflicts on a binary asset are all recurring costs, and they are paid by
 *     every consumer, forever, to save layout code written once here.
 *   - Anyone who wants a *designed* login screen is exactly the person who should build
 *     their own widget against the Blueprint API rather than inherit and fight ours.
 *     Shipping an asset invites editing the asset; shipping an API invites replacing it.
 *
 * The cost is real and worth naming: layout and styling live in code, where a designer
 * cannot reach them. HAM-182 scopes out styling beyond engine defaults, so that cost is
 * not actually being paid here — and a consumer who wants to pay it has the API.
 *
 * ## Lifecycle
 *
 * A real IModuleInterface rather than the core module's FDefaultModuleImpl. The core has
 * no startup work; a UI module plausibly will (a Slate style set is the usual one), and
 * having the hooks already in the right place is cheaper than retrofitting them.
 */
class FLoomaSceneSyncUIModule : public IModuleInterface
{
public:
    // --- IModuleInterface ---
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
