#include "Verify/ApexVerifyCard.h"

#include "ApexSim.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ApexVerifyCard
{
	bool Parse(const FString& Json, FApexVerifyCard& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("not JSON");
			return false;
		}
		FApexVerifyCard Card;
		Root->TryGetStringField(TEXT("block"), Card.Block);
		Root->TryGetStringField(TEXT("about"), Card.About);
		Root->TryGetStringField(TEXT("setup"), Card.Setup);
		double Minutes = Card.Minutes;
		Root->TryGetNumberField(TEXT("minutes"), Minutes);
		Card.Minutes = FMath::Max(0.0f, static_cast<float>(Minutes));
		Root->TryGetBoolField(TEXT("race"), Card.bRace);
		Root->TryGetStringField(TEXT("answers"), Card.AnswersPath);

		const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
		if (Root->TryGetArrayField(TEXT("checks"), Checks))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Checks)
			{
				const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Object.IsValid())
				{
					continue;
				}
				FApexVerifyCheck Check;
				Object->TryGetStringField(TEXT("item"), Check.Item);
				Object->TryGetStringField(TEXT("check"), Check.Check);
				Object->TryGetStringField(TEXT("detail"), Check.Detail);
				Object->TryGetStringField(TEXT("status"), Check.Status);
				Object->TryGetStringField(TEXT("before"), Check.Before);
				Object->TryGetStringArrayField(TEXT("answers"), Check.Answers);
				if (Check.Item.IsEmpty() || Check.Answers.IsEmpty())
				{
					continue;
				}
				Card.Checks.Add(MoveTemp(Check));
			}
		}
		if (Card.Block.IsEmpty() || Card.Checks.IsEmpty())
		{
			OutError = TEXT("no block or no checks");
			return false;
		}
		Out = MoveTemp(Card);
		return true;
	}

	bool Load(const FString& Path, FApexVerifyCard& Out, FString& OutError)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			OutError = FString::Printf(TEXT("cannot read %s"), *Path);
			return false;
		}
		return Parse(Json, Out, OutError);
	}

	FString SerialiseAnswers(const FApexVerifyCard& Card, const TArray<FApexVerifyAnswer>& Answers, EApexVerifyNext Next)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("block"), Card.Block);
		Root->SetStringField(TEXT("next"), Next == EApexVerifyNext::Next ? TEXT("next") : TEXT("stop"));
		TArray<TSharedPtr<FJsonValue>> List;
		for (int32 Index = 0; Index < Card.Checks.Num() && Index < Answers.Num(); ++Index)
		{
			const FApexVerifyAnswer& Answer = Answers[Index];
			if (Answer.IsEmpty())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("item"), Card.Checks[Index].Item);
			Object->SetStringField(TEXT("answer"), Answer.Answer);
			Object->SetStringField(TEXT("note"), Answer.Note.TrimStartAndEnd());
			Object->SetBoolField(TEXT("flag"), Answer.bFlag);
			Object->SetStringField(TEXT("at"), Answer.At);
			List.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("answers"), List);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	bool SaveAnswers(const FApexVerifyCard& Card, const TArray<FApexVerifyAnswer>& Answers, EApexVerifyNext Next)
	{
		if (Card.AnswersPath.IsEmpty())
		{
			return false;
		}
		const bool bSaved = FFileHelper::SaveStringToFile(SerialiseAnswers(Card, Answers, Next), *Card.AnswersPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bSaved)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Verify: could not write %s"), *Card.AnswersPath);
		}
		return bSaved;
	}

	FString CommandLinePath()
	{
		FString Path;
		FParse::Value(FCommandLine::Get(), TEXT("ApexVerify="), Path, /*bShouldStopOnSeparator*/ false);
		return Path.TrimQuotes();
	}
}
