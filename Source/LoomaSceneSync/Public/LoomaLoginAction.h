#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "LoomaAuthTypes.h"
#include "LoomaLoginAction.generated.h"

class ULoomaSceneSyncSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaLoginSucceeded, const FLoomaIdentity&, Identity);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoomaLoginFailed, const FString&, Error);

/**
 * Async Blueprint node: log in to the backend (POST /auth/login).
 *
 * On success the subsystem has already adopted the session by the time OnLoggedIn
 * fires — `Get Identity` and `Has Auth Token` are current on that pin, so a UI can
 * read them without a second round-trip.
 *
 * On failure *nothing else changes*: the scene-sync socket stays up, the scene stays
 * loaded, and whatever identity we had is still ours. An auth-enabled backend has to
 * remain usable by someone who cannot or will not log in, so a rejected password is
 * an event on this node and nowhere else.
 *
 * Error carries a message meant to be shown. It never distinguishes "no such user"
 * from "wrong password", because the backend does not either — `InvalidCredentials`
 * is one exception with one message by design (backend/app/auth/provider.py) — and a
 * client that guessed at the difference would hand back an account-enumeration
 * oracle the server went out of its way not to be.
 *
 * The request itself lives on ULoomaSceneSyncSubsystem::RequestLogin, not here: the
 * subsystem owns the token, and `Looma.Login` needs the same call. This node is the
 * Blueprint front door onto it, not a second implementation.
 */
UCLASS()
class LOOMASCENESYNC_API ULoomaLoginAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /** Logged in. The identity is the account's; the subsystem already holds the token. */
    UPROPERTY(BlueprintAssignable)
    FLoomaLoginSucceeded OnLoggedIn;

    /** Rejected, unreachable, or auth is switched off server-side. Error says which. */
    UPROPERTY(BlueprintAssignable)
    FLoomaLoginFailed OnFailed;

    /**
     * @param Username The account name, as registered.
     * @param Password The password. Held only for the duration of the request and
     *                 cleared as soon as it has been sent — it is never logged, and
     *                 it is not a UPROPERTY, so nothing serialises it either.
     */
    UFUNCTION(BlueprintCallable,
        meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
        Category = "Looma|Auth")
    static ULoomaLoginAction* Login(UObject* WorldContextObject, const FString& Username,
        const FString& Password);

    virtual void Activate() override;

private:
    TWeakObjectPtr<UObject> WorldContextObject;
    FString Username;
    FString Password;
};
