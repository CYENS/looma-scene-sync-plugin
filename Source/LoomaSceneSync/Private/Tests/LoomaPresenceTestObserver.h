#pragma once

#include "CoreMinimal.h"
#include "LoomaPresenceTypes.h"
#include "UObject/Object.h"
#include "LoomaPresenceTestObserver.generated.h"

// Dynamic multicast delegates require reflected callbacks. UHT cannot put a UCLASS
// behind WITH_DEV_AUTOMATION_TESTS; this private, transient type is instantiated only
// by the tests, matching the engine's own reflected automation observers.
UCLASS(Transient, NotBlueprintable)
class ULoomaPresenceTestObserver : public UObject
{
    GENERATED_BODY()

public:
    TFunction<void(const TArray<FLoomaClient>&)> OnClients;
    TFunction<void()> OnBorders;

    UFUNCTION()
    void ObserveClients(const TArray<FLoomaClient>& Clients)
    {
        if (OnClients)
        {
            OnClients(Clients);
        }
    }

    UFUNCTION()
    void ObserveBorders()
    {
        if (OnBorders)
        {
            OnBorders();
        }
    }
};
