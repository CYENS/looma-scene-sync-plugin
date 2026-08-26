#include "LoomaAuthTypes.h"

#include "Dom/JsonObject.h"

namespace
{
/** The first present of the given field names, as a string. */
FString GetIdentityStr(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys)
{
    for (const TCHAR* Key : Keys)
    {
        FString Value;
        // TryGetStringField answers false for a JSON null — FJsonValueNull does not
        // override TryGetString and the base returns false — so a `"user_id": null`
        // leaves Value untouched and we fall through to the next key, then to empty.
        // AsString() would have been the shorter idiom and the wrong one: it converts
        // rather than declining, and `str | None` fields are the whole reason this
        // helper exists.
        if (Obj->TryGetStringField(Key, Value))
        {
            return Value;
        }
    }
    return FString();
}

/** The first present of the given field names, as a bool; Fallback if none is present. */
bool GetIdentityBool(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, bool bFallback)
{
    for (const TCHAR* Key : Keys)
    {
        bool bValue = false;
        if (Obj->TryGetBoolField(Key, bValue))
        {
            return bValue;
        }
    }
    return bFallback;
}
} // namespace

ELoomaIdentityKind LoomaIdentityKindFromString(const FString& Kind)
{
    if (Kind == TEXT("guest")) { return ELoomaIdentityKind::Guest; }
    if (Kind == TEXT("user"))  { return ELoomaIdentityKind::User; }
    return ELoomaIdentityKind::Unknown;
}

FLoomaIdentity LoomaParseIdentity(const TSharedPtr<FJsonObject>& IdentityObj)
{
    FLoomaIdentity Identity;
    if (!IdentityObj.IsValid())
    {
        return Identity;
    }

    Identity.UserId      = GetIdentityStr(IdentityObj, { TEXT("user_id"), TEXT("userId") });
    Identity.DisplayName = GetIdentityStr(IdentityObj, { TEXT("display_name"), TEXT("displayName") });
    Identity.Kind        = LoomaIdentityKindFromString(GetIdentityStr(IdentityObj, { TEXT("kind") }));
    // Absent defaults to false, matching the schema's `is_admin: bool = False`. A
    // client may not infer the capability from anything else — not from being a
    // `user`, not from holding a token — because the backend re-checks it on every
    // admin route regardless of what we believe.
    Identity.bIsAdmin    = GetIdentityBool(IdentityObj, { TEXT("is_admin"), TEXT("isAdmin") }, false);
    return Identity;
}
