#include "LoomaGenerationTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LoomaWireConvert.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
// Read the first present of the given field names as a string.
FString GetStr(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys)
{
    for (const TCHAR* Key : Keys)
    {
        FString Value;
        if (Obj->TryGetStringField(Key, Value))
        {
            return Value;
        }
    }
    return FString();
}

// Read the first present of the given field names as a double (numbers only).
double GetNum(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, double Fallback)
{
    for (const TCHAR* Key : Keys)
    {
        double Value = 0.0;
        if (Obj->TryGetNumberField(Key, Value))
        {
            return Value;
        }
    }
    return Fallback;
}

// The shortest text that reads back as exactly `Value`. The engine's writer uses
// %.17g, which always reads back but turns 0.1 into 0.10000000000000001.
FString ShortestNumberText(double Value)
{
    FString Text = FString::Printf(TEXT("%.15g"), Value);
    if (FCString::Atod(*Text) != Value)
    {
        Text = FString::Printf(TEXT("%.16g"), Value);
    }
    if (FCString::Atod(*Text) != Value)
    {
        Text = FString::Printf(TEXT("%.17g"), Value);
    }
    return Text;
}

// A copy of `Value` whose numbers carry their shortest text, which the engine's
// writer then prints as it is. Clears bFinite on a NaN or an infinity.
TSharedPtr<FJsonValue> WithShortNumbers(const TSharedPtr<FJsonValue>& Value, bool& bFinite)
{
    if (!Value.IsValid())
    {
        return Value;
    }
    switch (Value->Type)
    {
    case EJson::Number:
    {
        const double Number = Value->AsNumber();
        if (!FMath::IsFinite(Number))
        {
            bFinite = false;
        }
        return MakeShared<FJsonValueNumberString>(ShortestNumberText(Number));
    }
    case EJson::Array:
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
        {
            Items.Add(WithShortNumbers(Item, bFinite));
        }
        return MakeShared<FJsonValueArray>(Items);
    }
    case EJson::Object:
    {
        const TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Value->AsObject()->Values)
        {
            Copy->SetField(Field.Key, WithShortNumbers(Field.Value, bFinite));
        }
        return MakeShared<FJsonValueObject>(Copy);
    }
    default:
        return Value;
    }
}
} // namespace

ELoomaJobState LoomaJobStateFromString(const FString& State)
{
    if (State == TEXT("queued"))         { return ELoomaJobState::Queued; }
    if (State == TEXT("running"))        { return ELoomaJobState::Running; }
    if (State == TEXT("awaiting_image")) { return ELoomaJobState::AwaitingImage; }
    if (State == TEXT("done"))           { return ELoomaJobState::Done; }
    if (State == TEXT("failed"))         { return ELoomaJobState::Failed; }
    if (State == TEXT("cancelled"))      { return ELoomaJobState::Cancelled; }
    return ELoomaJobState::Unknown;
}

FLoomaGenerationJob LoomaParseGenerationJob(const TSharedPtr<FJsonObject>& JobObj)
{
    FLoomaGenerationJob Job;
    if (!JobObj.IsValid())
    {
        return Job;
    }

    Job.JobId          = GetStr(JobObj, { TEXT("jobId"), TEXT("job_id") });
    Job.Prompt         = GetStr(JobObj, { TEXT("prompt") });
    Job.EnhancedPrompt = GetStr(JobObj, { TEXT("enhancedPrompt"), TEXT("enhanced_prompt") });
    Job.State          = LoomaJobStateFromString(GetStr(JobObj, { TEXT("state") }));
    Job.Progress       = static_cast<float>(GetNum(JobObj, { TEXT("progress") }, 0.0));
    Job.QueuePosition  = static_cast<int32>(GetNum(JobObj, { TEXT("queuePosition"), TEXT("queue_position") }, 0.0));
    Job.AssetId        = GetStr(JobObj, { TEXT("assetId"), TEXT("asset_id") });
    Job.AssetUrl       = GetStr(JobObj, { TEXT("assetUrl"), TEXT("asset_url") });
    // WS uses imageUrl (selected); REST list uses image_url; single-status uses selected_image_url.
    Job.SelectedImageUrl = GetStr(JobObj, {
        TEXT("imageUrl"), TEXT("image_url"),
        TEXT("selectedImageUrl"), TEXT("selected_image_url") });
    Job.Error          = GetStr(JobObj, { TEXT("error") });
    Job.CreatedAt      = GetStr(JobObj, { TEXT("createdAt"), TEXT("created_at") });
    Job.UpdatedAt      = GetStr(JobObj, { TEXT("updatedAt"), TEXT("updated_at") });

    const TArray<TSharedPtr<FJsonValue>>* ImagesArr = nullptr;
    if (JobObj->TryGetArrayField(TEXT("images"), ImagesArr))
    {
        for (const TSharedPtr<FJsonValue>& V : *ImagesArr)
        {
            const TSharedPtr<FJsonObject> ImgObj = V->AsObject();
            if (!ImgObj.IsValid())
            {
                continue;
            }
            FLoomaGeneratedImage Img;
            Img.Id   = GetStr(ImgObj, { TEXT("id") });
            Img.Url  = GetStr(ImgObj, { TEXT("url") });
            Img.Seed = static_cast<int32>(GetNum(ImgObj, { TEXT("seed") }, 0.0));
            Job.Images.Add(Img);
        }
    }

    const TSharedPtr<FJsonObject>* TransformObj = nullptr;
    if (JobObj->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj && (*TransformObj).IsValid())
    {
        Job.SuggestedTransform = LoomaWireToUe(*TransformObj);
        Job.bHasSuggestedTransform = true;
    }

    // Kept as text: nothing here reads it, and its keys are the submitter's, never
    // re-cased. Null, on a job's first event or a job sent without one, sets nothing.
    const TSharedPtr<FJsonObject>* MetaObj = nullptr;
    if (JobObj->TryGetObjectField(TEXT("meta"), MetaObj) && MetaObj && (*MetaObj).IsValid())
    {
        Job.MetaJson = LoomaWriteGenerationMeta(MakeShared<FJsonValueObject>(*MetaObj));
        Job.bHasMeta = true;
    }

    return Job;
}

FString LoomaWriteGenerationMeta(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid())
    {
        return FString();
    }
    bool bFinite = true;
    FString Text;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(WithShortNumbers(Value, bFinite), FString(), Writer);
    return Text;
}

bool LoomaReadGenerationMeta(const FString& MetaJson, TSharedPtr<FJsonObject>& OutMeta, FString& OutError)
{
    OutMeta.Reset();
    OutError.Reset();
    if (MetaJson.TrimStartAndEnd().IsEmpty())
    {
        return true;
    }

    TSharedPtr<FJsonObject> Parsed;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(MetaJson), Parsed) || !Parsed.IsValid())
    {
        // The backend answers anything but an object with a 422. Say so here,
        // before any request, with the shape it wants.
        OutError = FString::Printf(
            TEXT("MetaJson has to be one JSON object, such as {\"unreal\":{\"environment\":\"LVL_Demo\"}}, and this is not: %s"),
            *MetaJson.Left(120));
        return false;
    }

    bool bFinite = true;
    const TSharedPtr<FJsonValue> Short = WithShortNumbers(MakeShared<FJsonValueObject>(Parsed), bFinite);
    if (!bFinite)
    {
        OutError = TEXT("MetaJson holds a number too large for JSON, which the backend refuses");
        return false;
    }
    OutMeta = Short->AsObject();
    return true;
}
