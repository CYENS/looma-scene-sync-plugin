#include "LoomaSceneSyncUIModule.h"

#include "LoomaSceneSyncLog.h"
#include "Modules/ModuleManager.h"

void FLoomaSceneSyncUIModule::StartupModule()
{
    // Nothing to set up yet — the widget arrives in the next commit, and a style set
    // with it if it needs one.
    //
    // The log line is not decoration. LogLoomaSync is declared LOOMASCENESYNC_API by the
    // core module, so referencing it here is what forces this module to actually link
    // against LoomaSceneSync: a scaffold with no uses of its dependency would compile
    // just as happily with the dependency mis-declared, and the mistake would surface in
    // the next commit as a pile of unresolved externals instead of here. Verbose, so a
    // launch stays quiet.
    UE_LOG(LogLoomaSync, Verbose, TEXT("LoomaSceneSyncUI module started"));
}

void FLoomaSceneSyncUIModule::ShutdownModule()
{
    UE_LOG(LogLoomaSync, Verbose, TEXT("LoomaSceneSyncUI module shut down"));
}

// One log category for the whole plugin, defined once in the core module — see
// LoomaSceneSyncLog.h. This module deliberately does not add a second one: a reader
// chasing a login problem should not have to know which half of the plugin logged it.
IMPLEMENT_MODULE(FLoomaSceneSyncUIModule, LoomaSceneSyncUI)
