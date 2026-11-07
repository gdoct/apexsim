#include "ApexTestCommon.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Verify/ApexVerifyCard.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexVerifyCardTest,
	"ApexSim.Verify.Card",
	ApexTestFlags)

bool FApexVerifyCardTest::RunTest(const FString& Parameters)
{
	// The card as scripts/verify/run_checks.py writes it (write_verify_card).
	const FString Json = TEXT(R"({
		"block": "a-monza", "about": "Baseline", "setup": "Cockpit view.", "minutes": 3.5, "race": true,
		"answers": "C:/out/verify-answers.json",
		"checks": [
			{"item": "v-sit-a-monza-letgo", "check": "Letting go", "answers": ["OK", "rings", "holds"], "status": "open", "before": ""},
			{"item": "v-wheel-crown", "check": "The crown", "detail": "Through the rim.", "answers": ["OK", "not right", "can't tell"],
			 "status": "not-right", "before": "not right"},
			{"item": "v-no-answers", "check": "Dropped", "answers": []}
		]})");
	FApexVerifyCard Card;
	FString Error;
	TestTrue(TEXT("the runner's card parses"), ApexVerifyCard::Parse(Json, Card, Error));
	TestEqual(TEXT("block"), Card.Block, FString(TEXT("a-monza")));
	TestEqual(TEXT("minutes"), Card.Minutes, 3.5f);
	TestTrue(TEXT("a race block"), Card.bRace);
	TestEqual(TEXT("a check with no answer words is left out"), Card.Checks.Num(), 2);
	TestEqual(TEXT("answer words"), Card.Checks[0].Answers.Num(), 3);
	TestEqual(TEXT("detail"), Card.Checks[1].Detail, FString(TEXT("Through the rim.")));
	TestEqual(TEXT("before"), Card.Checks[1].Before, FString(TEXT("not right")));

	FApexVerifyCard Empty;
	TestFalse(TEXT("no checks is no card"), ApexVerifyCard::Parse(TEXT(R"({"block": "x", "checks": []})"), Empty, Error));
	TestFalse(TEXT("not JSON"), ApexVerifyCard::Parse(TEXT("checks:"), Empty, Error));

	// The answers: only the answered checks, keyed by item, with the choice after.
	TArray<FApexVerifyAnswer> Answers;
	Answers.SetNum(2);
	Answers[1].Answer = TEXT("not right");
	Answers[1].Note = TEXT("  too light  ");
	Answers[1].bFlag = true;
	Answers[1].At = TEXT("2026-11-07T20:14:00");
	TSharedPtr<FJsonObject> Written;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ApexVerifyCard::SerialiseAnswers(Card, Answers, EApexVerifyNext::Next));
	TestTrue(TEXT("the answers are JSON"), FJsonSerializer::Deserialize(Reader, Written) && Written.IsValid());
	if (!Written.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("next"), Written->GetStringField(TEXT("next")), FString(TEXT("next")));
	const TArray<TSharedPtr<FJsonValue>>& List = Written->GetArrayField(TEXT("answers"));
	TestEqual(TEXT("an unanswered check is not written"), List.Num(), 1);
	if (List.Num() == 1)
	{
		const TSharedPtr<FJsonObject> One = List[0]->AsObject();
		TestEqual(TEXT("item"), One->GetStringField(TEXT("item")), FString(TEXT("v-wheel-crown")));
		TestEqual(TEXT("answer"), One->GetStringField(TEXT("answer")), FString(TEXT("not right")));
		TestEqual(TEXT("the note trimmed"), One->GetStringField(TEXT("note")), FString(TEXT("too light")));
		TestTrue(TEXT("flag"), One->GetBoolField(TEXT("flag")));
	}
	return true;
}

#endif
