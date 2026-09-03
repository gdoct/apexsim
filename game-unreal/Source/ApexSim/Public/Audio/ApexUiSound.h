#pragma once

#include "CoreMinimal.h"

#include "ApexUiSound.generated.h"

/**
 * The menu's vocabulary of cues. Every sound the shell makes is one of these,
 * so a screen never picks a tone — it says what happened and the audio
 * subsystem decides how that sounds.
 */
UENUM(BlueprintType)
enum class EApexUiSound : uint8
{
	/** Focus moved to another control. */
	Move,
	/** A control was activated, a screen entered, a choice confirmed. */
	Accept,
	/** A step back: Escape, B, a Back button, an overlay closing. */
	Back,
	/** The press did nothing on purpose: a locked row, an edge with nothing past it. */
	Denied,
	/** A continuous value stepped: sliders and dropdowns. */
	Adjust,
	/** A toast with news. */
	Notice,
	/** A toast with bad news. */
	Error,

	Count UMETA(Hidden),
};

/**
 * The cues, synthesised.
 *
 * There are no sound assets in the project and, for a handful of short menu
 * tones, none are wanted: a wave file is a binary blob nobody can review,
 * while a tone described in code is diffable, sample-rate independent and
 * cannot go missing from a cook. Everything here is pure arithmetic on a
 * buffer, callable from any thread and covered by an automation test.
 */
namespace ApexUiSynth
{
	/** One note of a cue: a pitch, how long it rings, and how loud. */
	struct FNote
	{
		float Hz = 440.0f;
		float Seconds = 0.1f;
		float Gain = 0.4f;
	};

	/** The notes a cue is made of, in playing order. */
	APEXSIM_API TArray<FNote> NotesFor(EApexUiSound Sound);

	/**
	 * Renders a cue as mono float samples at the given rate. Every cue starts
	 * and ends at silence and never exceeds ±1, whatever the rate.
	 */
	APEXSIM_API void Render(EApexUiSound Sound, float SampleRate, TArray<float>& OutSamples);

	/** How long the cue lasts, in seconds. Same answer as Render's length over the rate. */
	APEXSIM_API float DurationSeconds(EApexUiSound Sound);
}
