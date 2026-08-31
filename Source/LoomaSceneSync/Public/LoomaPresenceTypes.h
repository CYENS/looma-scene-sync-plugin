#pragma once

#include "CoreMinimal.h"
#include "LoomaPresenceTypes.generated.h"

class FJsonObject;

/**
 * What kind of participant one roster entry is, mirroring the `kind` field of the
 * `clients` message exactly (docs/scene-format.md, "Who else is in the room").
 *
 * Deliberately NOT ELoomaIdentityKind, though the two have the same two named
 * values. They answer different questions and, more to the point, have different
 * totality: an identity may be *unestablished* — nothing has told us who we are yet
 * — which is why that enum carries Unknown and treats an unparseable `kind` as
 * "still no answer". A roster entry is always an answer; the hub resolved it
 * server-side before sending it, and the contract's rule is flatly "guest unless the
 * hub said user". Folding Unknown in here would put a fourth state into a Blueprint
 * switch that the wire can never produce, and would tempt a consumer to render a
 * third thing for it — while the correct behaviour for a `kind` this build predates
 * is the opposite of the identity one: fall back to Guest, because showing a client
 * as an account it may not hold is the failure this field exists to prevent.
 *
 * Guest is the zero value so a default-constructed FLoomaClient is already
 * contract-correct rather than needing the parser to have run.
 */
UENUM(BlueprintType)
enum class ELoomaClientKind : uint8
{
    /** No account, or a hub too old to say. The default, and the safe answer. */
    Guest UMETA(DisplayName = "Guest"),

    /** The hub resolved this socket to a registered account. */
    User UMETA(DisplayName = "User")
};

/**
 * One client in the room, from the hub's `clients` roster.
 *
 * **Presence, not scene state.** It is never merged into the scene document, never
 * saved, never sent in a `scene`, and it dies with the socket — see
 * ULoomaSceneSyncSubsystem::ClearPresence.
 *
 * Nothing here is ours to invent. The colour is the server's to assign (a client
 * that picks its own is wrong), the display name is server-resolved, and `kind` is
 * whatever the hub decided. Every field below is either what the hub said or a
 * documented fallback for it never having said anything.
 */
USTRUCT(BlueprintType)
struct FLoomaClient
{
    GENERATED_BODY()

    /** That client's `clientId`, exactly as it sent in its `hello`. Empty is not a valid entry. */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Presence")
    FString Id;

    /**
     * The hub's colour for this client, ready to drive a material or an outline.
     *
     * The wire is an sRGB hex string, so this is the sRGB->linear conversion of it —
     * the same one FColor-to-FLinearColor does everywhere else in UE. Feeding the raw
     * bytes in as linear would wash every colour out, and the point of a per-client
     * colour is that two of them are told apart at a glance.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Presence")
    FLinearColor Color = FLinearColor::Gray;

    /**
     * The same colour as the hub wrote it, normalised to lowercase `#rrggbb`.
     *
     * Kept alongside the FLinearColor rather than derived on demand, because the two
     * do not round-trip: going back through linear->sRGB->hex can land a digit off,
     * and this is the string a log line, a HUD label or a name-tag should show, since
     * it is the one everyone else in the room sees too. `Looma.Room` prints it.
     * Always six digits — a `#rgb` from an older hub is expanded here, so a consumer
     * comparing strings never has to handle both forms.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Presence")
    FString ColorHex;

    /**
     * That client's `hello` role — `web`, `unreal`, `unity`, … — or `unknown` if it
     * sent none. A label to display; the hub never acts on it and neither should we.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Presence")
    FString Role;

    /**
     * The name to show (HAM-172), server-resolved: a registered account's, or a
     * guest's `Guest-xxxxxx`.
     *
     * **Empty against a hub old enough not to send the field at all**, and empty is
     * to be shown as a fallback of the consumer's choosing (role plus an id prefix is
     * what the web does) — never filled in here. Fabricating a name would put a
     * string in the room that no other client is showing.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Presence")
    FString DisplayName;

    UPROPERTY(BlueprintReadOnly, Category = "Looma|Presence")
    ELoomaClientKind Kind = ELoomaClientKind::Guest;

    /**
     * The node ids that client currently has selected — its **whole** selection, as
     * last sent, never a delta.
     *
     * This is what hydrates a joiner: nothing replays the `selection` messages we
     * were not connected for. It is refreshed only on a join or a leave, so it goes
     * stale between rosters and the `selection` message is what keeps it current
     * (step 2). Order is that client's, not ours; it is not sorted here.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Presence")
    TArray<FString> Selection;

    /**
     * Field-wise, so a roster that did not actually move does not fire an event —
     * the same reason FLoomaIdentity has one. ColorHex is compared rather than Color
     * because it is the exact thing the hub sent; the linear value is derived from it
     * and cannot differ when it does not.
     */
    bool operator==(const FLoomaClient& Other) const
    {
        return Kind == Other.Kind
            && Id == Other.Id
            && ColorHex == Other.ColorHex
            && Role == Other.Role
            && DisplayName == Other.DisplayName
            && Selection == Other.Selection;
    }

    bool operator!=(const FLoomaClient& Other) const
    {
        return !(*this == Other);
    }
};

/**
 * The colour for a client we hold no valid colour for: a neutral grey, chosen so it
 * reads as "no colour was given" rather than as somebody's assigned one.
 *
 * The same `#bbbbbb` the web client falls back to (frontend/src/sync/presence.js),
 * and shared with it on purpose — a client drawn grey here and grey there is one
 * client with a missing colour, not two clients with different bugs. Step 2 needs it
 * for a second reason the roster does not: a `selection` can legitimately arrive
 * from a client the roster has not introduced yet, and grey for one message is
 * self-correcting where dropping the claim would lose it for good.
 */
LOOMASCENESYNC_API extern const TCHAR* const LoomaFallbackClientColorHex;

/**
 * Read a hub-assigned colour off the wire into both forms.
 *
 * `OutHex` is lowercase `#rrggbb`, `OutColor` its linear value. Anything that is not
 * `#rgb` or `#rrggbb` — a missing field, or a client that invented a colour the hub
 * would never send — yields LoomaFallbackClientColorHex rather than a guess. There is
 * no third outcome: we never pick a per-client colour of our own, because a colour is
 * only meaningful if every client in the room agrees on it, and only the hub can make
 * them agree.
 *
 * Validated here rather than by handing the string to FColor::FromHex, which is
 * lenient in exactly the wrong direction: it accepts a missing `#` and 4- and 8-digit
 * forms, and answers black for garbage — indistinguishable from a real black.
 */
LOOMASCENESYNC_API void LoomaParseClientColor(const FString& Wire, FString& OutHex, FLinearColor& OutColor);

/**
 * Parse one entry of a `clients` roster.
 *
 * Returns a client with an empty Id for anything unreadable, which every caller must
 * drop — an entry with no id names nobody, and a border has to be attributable.
 */
LOOMASCENESYNC_API FLoomaClient LoomaParseClient(const TSharedPtr<FJsonObject>& ClientObj);
