#include "LoomaGenerationLibrary.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LoomaSceneSyncLog.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool ULoomaGenerationLibrary::MakeGenerationMeta(const TMap<FString, FString>& Values, FString& MetaJson)
{
    MetaJson.Reset();
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    for (const TPair<FString, FString>& Entry : Values)
    {
        TArray<FString> Keys;
        Entry.Key.ParseIntoArray(Keys, TEXT("."), /*InCullEmpty=*/false);
        if (Keys.IsEmpty() || Keys.Contains(FString()))
        {
            UE_LOG(LogLoomaSync, Warning,
                TEXT("Make Generation Meta: '%s' is not a path of keys joined by dots, such as unreal.environment"),
                *Entry.Key);
            return false;
        }

        // Walk down the groups the path names, making the ones not there yet.
        TSharedRef<FJsonObject> Group = Root;
        for (int32 Index = 0; Index < Keys.Num(); ++Index)
        {
            const TSharedPtr<FJsonValue> Existing = Group->TryGetField(Keys[Index]);
            const bool bLeaf = Index == Keys.Num() - 1;
            if (!bLeaf && Existing.IsValid() && Existing->Type == EJson::Object)
            {
                Group = Existing->AsObject().ToSharedRef();
            }
            else if (Existing.IsValid())
            {
                UE_LOG(LogLoomaSync, Warning,
                    TEXT("Make Generation Meta: '%s' runs into another path in the map at '%s'. One key cannot hold both a value and a group"),
                    *Entry.Key, *Keys[Index]);
                return false;
            }
            else if (bLeaf)
            {
                Group->SetStringField(Keys[Index], Entry.Value);
            }
            else
            {
                const TSharedRef<FJsonObject> Child = MakeShared<FJsonObject>();
                Group->SetObjectField(Keys[Index], Child);
                Group = Child;
            }
        }
    }

    if (Root->Values.Num() > 0)
    {
        MetaJson = LoomaWriteGenerationMeta(MakeShared<FJsonValueObject>(Root));
    }
    return true;
}

bool ULoomaGenerationLibrary::GetGenerationMetaString(const FLoomaGenerationJob& Job, const FString& Path,
    FString& Value)
{
    Value.Reset();
    TSharedPtr<FJsonObject> Meta;
    if (!Job.bHasMeta
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Job.MetaJson), Meta)
        || !Meta.IsValid())
    {
        return false;
    }

    TArray<FString> Keys;
    Path.ParseIntoArray(Keys, TEXT("."), /*InCullEmpty=*/false);
    if (Keys.IsEmpty())
    {
        return false;
    }

    TSharedPtr<FJsonValue> Found = MakeShared<FJsonValueObject>(Meta);
    for (const FString& Key : Keys)
    {
        const TSharedPtr<FJsonObject>* Group = nullptr;
        if (!Found->TryGetObject(Group) || !Group || !Group->IsValid())
        {
            return false;
        }
        Found = (*Group)->TryGetField(Key);
        if (!Found.IsValid())
        {
            return false;
        }
    }

    switch (Found->Type)
    {
    case EJson::Null:
        return false;
    case EJson::String:
        Value = Found->AsString();
        return true;
    default:
        Value = LoomaWriteGenerationMeta(Found);
        return true;
    }
}
