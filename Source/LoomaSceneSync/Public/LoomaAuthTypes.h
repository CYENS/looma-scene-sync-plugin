#pragma once

#include "CoreMinimal.h"
#include "LoomaAuthTypes.generated.h"

class FJsonObject;

/**
 * What kind of participant an identity is, mirroring the backend's
 * `Identity.kind` (`Literal["guest", "user"]` in backend/app/auth/provider.py).
 *
 * Unknown is ours, not the backend's, and it is load-bearing in two places:
 * before anything has established who we are, and for a `kind` we cannot parse.
 * A future provider (the provider docstring names Firebase/OIDC as the intended
 * extension) could introduce a third kind, and a client that silently folded an
 * unrecognised one into Guest would quietly under-report a real account. Same rule
 * as ELoomaAuthState: an answer we cannot read is not an answer.
 */
UENUM(BlueprintType)
enum class ELoomaIdentityKind : uint8
{
    /** Nothing has told us yet, or the backend named a kind this build predates. */
    Unknown UMETA(DisplayName = "Unknown"),

    /** No account: the hub named us `Guest-xxxxxx`, or we suggested a display name. */
    Guest UMETA(DisplayName = "Guest"),

    /** A logged-in account. */
    User UMETA(DisplayName = "User")
};

/**
 * Who the backend says we are — its `IdentityOut` (backend/app/auth/routes.py),
 * which the provider docstring calls "the ONLY shape a consumer ever sees". It
 * never carries a password, a hash, or a token, and neither does this.
 *
 * Note the two transports disagree on case, exactly as they do for generation jobs:
 * the REST surface (`/auth/login`, `/auth/me`) is Pydantic field names, so
 * snake_case — `user_id`, `display_name`, `is_admin` — while the WS roster is
 * camelCase. LoomaParseIdentity accepts both for the same reason
 * LoomaParseGenerationJob does: one struct, one parser, no per-transport variant to
 * keep in step.
 */
USTRUCT(BlueprintType)
struct FLoomaIdentity
{
    GENERATED_BODY()

    /**
     * The account id, empty for a guest. `user_id` is `str | None` in the schema —
     * a guest has none — so this is empty rather than sentinel-valued, and the
     * parser reads a JSON null as "absent" rather than as the text "null".
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Auth")
    FString UserId;

    /**
     * The name a roster shows. For an account it is the registered username; for a
     * guest it is `Guest-xxxxxx` derived from the WS clientId, or a suggested name
     * that survived cleaning.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Auth")
    FString DisplayName;

    UPROPERTY(BlueprintReadOnly, Category = "Looma|Auth")
    ELoomaIdentityKind Kind = ELoomaIdentityKind::Unknown;

    /**
     * The one capability the backend exposes, not a role system (HAM-174). Advisory
     * for a client: every admin route re-checks it server-side, so a UI may use this
     * to decide what to bother drawing and nothing more.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Looma|Auth")
    bool bIsAdmin = false;

    /** Field-wise, so an identity that did not actually move does not fire an event. */
    bool operator==(const FLoomaIdentity& Other) const
    {
        return Kind == Other.Kind
            && bIsAdmin == Other.bIsAdmin
            && UserId == Other.UserId
            && DisplayName == Other.DisplayName;
    }

    bool operator!=(const FLoomaIdentity& Other) const
    {
        return !(*this == Other);
    }
};

/** Map the backend's `kind` string ("guest" / "user") to the enum; anything else is Unknown. */
LOOMASCENESYNC_API ELoomaIdentityKind LoomaIdentityKindFromString(const FString& Kind);

/**
 * Parse an `IdentityOut` from either transport — snake_case (REST: user_id,
 * display_name, is_admin) or camelCase (WS: userId, displayName, isAdmin).
 *
 * Returns an identity with Kind == Unknown for an invalid or unreadable object,
 * which is what every caller must treat as "we still do not know", never as a guest.
 * In particular `GET /auth/me` NEVER answers 401: an unauthenticated caller gets a
 * guest identity with a 200, so the only way to tell a live session from a dead one
 * is to read Kind. A status-code check there would trust a revoked token forever.
 */
LOOMASCENESYNC_API FLoomaIdentity LoomaParseIdentity(const TSharedPtr<FJsonObject>& IdentityObj);
