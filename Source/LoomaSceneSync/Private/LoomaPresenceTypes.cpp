#include "LoomaPresenceTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

const TCHAR* const LoomaFallbackClientColorHex = TEXT("#bbbbbb");

namespace
{
/**
 * `#rgb` or `#rrggbb`, case-insensitive, expanded to six lowercase digits. Empty if
 * the string is anything else.
 *
 * The hub only ever sends lowercase `#rrggbb`; the short form is accepted because the
 * web client accepts it and the two halves of one protocol should not disagree about
 * what is a valid colour. Written out rather than run through FRegexPattern: this is
 * on the path of every roster entry, and the check is four characters of arithmetic.
 */
FString NormalizeColorHex(const FString& Wire)
{
    if (Wire.Len() != 4 && Wire.Len() != 7)
    {
        return FString();
    }
    if (Wire[0] != TEXT('#'))
    {
        return FString();
    }
    FString Digits;
    Digits.Reserve(6);
    for (int32 i = 1; i < Wire.Len(); ++i)
    {
        const TCHAR C = FChar::ToLower(Wire[i]);
        if (!FChar::IsHexDigit(C))
        {
            return FString();
        }
        Digits.AppendChar(C);
        // `#rgb` means each digit doubled, not each digit padded — #abc is #aabbcc.
        if (Wire.Len() == 4)
        {
            Digits.AppendChar(C);
        }
    }
    return FString(TEXT("#")) + Digits;
}
} // namespace

void LoomaParseClientColor(const FString& Wire, FString& OutHex, FLinearColor& OutColor)
{
    OutHex = NormalizeColorHex(Wire);
    if (OutHex.IsEmpty())
    {
        OutHex = LoomaFallbackClientColorHex;
    }
    // sRGB->linear, because the hex is a colour someone picked by eye in a browser and
    // UE renders in linear space. FromSRGBColor rather than the FLinearColor(FColor)
    // constructor purely for being explicit about which direction is meant — they do
    // the same thing, and getting the direction wrong is a bug that looks like a
    // washed-out palette rather than like an error.
    OutColor = FLinearColor::FromSRGBColor(FColor::FromHex(OutHex));
}

FLoomaClient LoomaParseClient(const TSharedPtr<FJsonObject>& ClientObj)
{
    FLoomaClient Client;
    if (!ClientObj.IsValid())
    {
        return Client;
    }

    // An entry with no id is unusable rather than merely incomplete — see the header.
    // Everything below is still filled in, so a caller that logs the reject has
    // something to log.
    ClientObj->TryGetStringField(TEXT("id"), Client.Id);

    FString ColorWire;
    ClientObj->TryGetStringField(TEXT("color"), ColorWire);
    LoomaParseClientColor(ColorWire, Client.ColorHex, Client.Color);

    // `unknown` is the hub's own word for "that client sent no role", so an absent
    // field lands on the same value a present one would — one less case for a UI.
    if (!ClientObj->TryGetStringField(TEXT("role"), Client.Role) || Client.Role.IsEmpty())
    {
        Client.Role = TEXT("unknown");
    }

    // Left empty when absent, and deliberately not defaulted to anything: an old hub
    // omits the field entirely, and only the consumer knows what to draw instead.
    ClientObj->TryGetStringField(TEXT("displayName"), Client.DisplayName);

    FString KindWire;
    ClientObj->TryGetStringField(TEXT("kind"), KindWire);
    // Guest unless the hub said exactly "user" — never fabricate an account for a
    // guest. Note this is the opposite fallback from LoomaIdentityKindFromString,
    // which answers Unknown; see ELoomaClientKind for why the two differ.
    Client.Kind = (KindWire == TEXT("user")) ? ELoomaClientKind::User : ELoomaClientKind::Guest;

    const TArray<TSharedPtr<FJsonValue>>* Selection = nullptr;
    if (ClientObj->TryGetArrayField(TEXT("selection"), Selection) && Selection)
    {
        Client.Selection.Reserve(Selection->Num());
        for (const TSharedPtr<FJsonValue>& Value : *Selection)
        {
            FString NodeId;
            // A non-string entry is skipped rather than coerced: AsString() would turn
            // a number into a node id that matches nothing, which reads downstream as a
            // claim on a node that has not spawned yet — the one case we are required
            // to keep rather than drop.
            if (Value.IsValid() && Value->TryGetString(NodeId) && !NodeId.IsEmpty())
            {
                Client.Selection.Add(NodeId);
            }
        }
    }
    return Client;
}
