#include "LoomaLoginAction.h"

#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "LoomaSceneSyncSubsystem.h"

ULoomaLoginAction* ULoomaLoginAction::Login(UObject* WorldContextObject, const FString& Username,
    const FString& Password)
{
    ULoomaLoginAction* Action = NewObject<ULoomaLoginAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Username = Username;
    Action->Password = Password;
    return Action;
}

void ULoomaLoginAction::Activate()
{
    UGameInstance* GameInstance = WorldContextObject.IsValid()
        ? UGameplayStatics::GetGameInstance(WorldContextObject.Get())
        : nullptr;
    ULoomaSceneSyncSubsystem* SyncSubsystem = GameInstance
        ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>()
        : nullptr;

    if (!SyncSubsystem)
    {
        OnFailed.Broadcast(TEXT("LoomaSceneSync subsystem unavailable"));
        SetReadyToDestroy();
        return;
    }

    // Keep this action alive across the async HTTP round-trip.
    RegisterWithGameInstance(GameInstance);

    // Weak, so a login that outlives the node (a level change mid-request, say) is
    // still adopted by the subsystem — it just has nobody left to tell.
    TWeakObjectPtr<ULoomaLoginAction> WeakThis(this);
    SyncSubsystem->RequestLogin(Username, Password,
        [WeakThis](bool bSuccess, const FLoomaIdentity& Identity, const FString& Error) {
            ULoomaLoginAction* Action = WeakThis.Get();
            if (!Action)
            {
                return;
            }
            if (bSuccess)
            {
                Action->OnLoggedIn.Broadcast(Identity);
            }
            else
            {
                Action->OnFailed.Broadcast(Error);
            }
            Action->SetReadyToDestroy();
        });

    // Sent — so there is no reason to keep it. Nothing here can be asked to retry
    // (a Blueprint calls the node again, with the password it still holds), so the
    // only thing holding a copy longer would be doing it for nothing.
    Password.Empty();
}
