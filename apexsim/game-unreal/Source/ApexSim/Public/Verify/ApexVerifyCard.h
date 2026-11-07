#pragma once

#include "CoreMinimal.h"

/**
 * The verify card of a sitting block (docs/verifier.md, "The verify card"):
 * what scripts/verify/run_checks.py --sit hands the game with -ApexVerify=,
 * and the answers the game writes back for it.
 *
 * The runner writes the card (the block's brief, its driving time, its
 * checks with their answer words); the game asks the checks once the time
 * is up and writes the answers file the card names, on every change, so a
 * game closed half way still leaves what was answered. The runner turns
 * the answers into reviews in the verifier's items file.
 */
struct FApexVerifyCheck
{
	/** The verifier item the answer reviews (v-sit-<block>-<check>, or the block's other items). */
	FString Item;
	FString Check;
	/** The item's `what` for an item that is not a sitting check; empty for one. */
	FString Detail;
	/** The words the check is answered with; the first one confirms. */
	TArray<FString> Answers;
	/** The item's status and the answer it was last given, for "answered before". */
	FString Status;
	FString Before;
};

struct FApexVerifyAnswer
{
	FString Answer;
	FString Note;
	bool bFlag = false;
	/** Local time of the answer, "2026-11-07T20:14:00". */
	FString At;

	bool IsEmpty() const { return Answer.IsEmpty() && Note.TrimStartAndEnd().IsEmpty() && !bFlag; }
};

/** What the player chose after the card: the runner's next block, or stop there. */
enum class EApexVerifyNext : uint8
{
	Next,
	Stop,
};

struct FApexVerifyCard
{
	FString Block;
	FString About;
	FString Setup;
	/** Driving time before the card is due. */
	float Minutes = 10.0f;
	/** The block drives a race (-ApexAutoRace): the clock starts with the race view, and leaving it brings the card. */
	bool bRace = false;
	/** Where the answers go. */
	FString AnswersPath;
	TArray<FApexVerifyCheck> Checks;
};

namespace ApexVerifyCard
{
	/** The card from its JSON; false (and why) when it is not one. */
	APEXSIM_API bool Parse(const FString& Json, FApexVerifyCard& Out, FString& OutError);
	APEXSIM_API bool Load(const FString& Path, FApexVerifyCard& Out, FString& OutError);

	/** The answers file: `{block, next, answers: [{item, answer, note, flag, at}]}`, one per answered check. */
	APEXSIM_API FString SerialiseAnswers(const FApexVerifyCard& Card, const TArray<FApexVerifyAnswer>& Answers, EApexVerifyNext Next);
	APEXSIM_API bool SaveAnswers(const FApexVerifyCard& Card, const TArray<FApexVerifyAnswer>& Answers, EApexVerifyNext Next);

	/** -ApexVerify=<path>, or empty. */
	APEXSIM_API FString CommandLinePath();
}
