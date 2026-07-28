#include "LoomaWireConvert.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FTransform LoomaWireToUe(const TSharedPtr<FJsonObject>& T)
{
    if (!T.IsValid())
    {
        return FTransform::Identity;
    }

    auto Num = [](const TArray<TSharedPtr<FJsonValue>>& A, int32 I, double Fallback) {
        return A.IsValidIndex(I) ? A[I]->AsNumber() : Fallback;
    };
    const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Q = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* S = nullptr;
    T->TryGetArrayField(TEXT("p"), P);
    T->TryGetArrayField(TEXT("q"), Q);
    T->TryGetArrayField(TEXT("s"), S);

    FVector Location = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
    if (P)
    {
        Location = FVector(-Num(*P, 2, 0) * 100.0, Num(*P, 0, 0) * 100.0, Num(*P, 1, 0) * 100.0);
    }
    if (Q)
    {
        Rotation = FQuat(Num(*Q, 2, 0), -Num(*Q, 0, 0), -Num(*Q, 1, 0), Num(*Q, 3, 1));
        Rotation.Normalize();
    }
    if (S)
    {
        Scale = FVector(Num(*S, 2, 1), Num(*S, 0, 1), Num(*S, 1, 1));
    }
    return FTransform(Rotation, Location, Scale);
}

TSharedRef<FJsonObject> LoomaUeToWire(const FTransform& T)
{
    const FVector L = T.GetLocation();
    const FQuat Q = T.GetRotation();
    const FVector S = T.GetScale3D();

    auto Arr = [](std::initializer_list<double> Values) {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (double V : Values)
        {
            Out.Add(MakeShared<FJsonValueNumber>(V));
        }
        return Out;
    };

    // Inverse of the map above: UE (X, Y, Z) -> wire (Y, Z, -X).
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetArrayField(TEXT("p"), Arr({ L.Y / 100.0, L.Z / 100.0, -L.X / 100.0 }));
    Obj->SetArrayField(TEXT("q"), Arr({ -Q.Y, -Q.Z, Q.X, Q.W }));
    Obj->SetArrayField(TEXT("s"), Arr({ S.Y, S.Z, S.X }));
    return Obj;
}
