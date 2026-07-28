#include "LoomaGenerationTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LoomaWireConvert.h"

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

    return Job;
}
