#pragma once

#include "CoreMinimal.h"

struct FApexHudData;

/**
 * The spotter and the race engineer on the radio: which voice line to say
 * when, and in what order. Pure logic, driven by the HUD's data points (the
 * radar, race control's flags, the pit plan, fuel, tyres, the lap count), so
 * the tests can play a race at it. UApexRadioSubsystem plays what it picks.
 *
 * The lines are the files apexsim-content/audio/radio/<id>.wav, rendered from
 * that folder's lines.yml (docs/game/audio.md, "Radio").
 */
enum class EApexRadioChannel : uint8
{
	Spotter,
	Engineer,
};

/** What the radio reacts to: the driven car's slice of the HUD data. */
struct APEXSIM_API FApexRadioInputs
{
	/** A car of the player's own on track in a live session; nothing is said otherwise. */
	bool bDriving = false;
	/** `session.mode`: `race`, `qualifying`, `practice`, `grid`, `countdown`, ... */
	FString Mode;
	float SpeedKph = 0.0f;
	bool bInPitLane = false;
	bool bInGarage = false;
	/** `radar.left` / `radar.right`: a car alongside on that side. */
	bool bLeft = false;
	bool bRight = false;
	/** `flag.yellow`: 0 none, 1 single, 2 double. */
	int32 Yellow = 0;
	bool bBlue = false;
	/** `race.phase`, empty while there is none. */
	FString Phase;
	bool bPitRequested = false;
	/** `pit.stops_owed`; -1 without a rule. */
	int32 StopsOwed = -1;
	/** `lap.laps_left`; -1 without a race distance. */
	int32 LapsLeft = -1;
	/** `fuel.state`: `ok`, `short`, `critical`, `empty`, `unknown`. */
	FString FuelState;
	/** `fuel.laps`; negative while unknown. */
	float FuelLaps = -1.0f;
	/** `tyre.wear_max_pct`; negative while unknown. */
	float TyreWearMaxPct = -1.0f;
	/** `lap.current`. */
	int32 Lap = 0;
	/** `lap.final`. */
	bool bFinalLap = false;
	/** `car.finish_position`: 0 until the car takes the flag. */
	int32 FinishPosition = 0;
};

namespace ApexRadio
{
	/** How long a line keeps, how urgent it is and what it supersedes. */
	struct FLineSpec
	{
		EApexRadioChannel Channel = EApexRadioChannel::Engineer;
		/** Higher plays first; a spotter call at SpotterInterruptPriority cuts an engineer line short. */
		int32 Priority = 0;
		/** Dropped unsaid once it has waited this long: a late "car left" is wrong. */
		double MaxWaitS = 20.0;
		/** Not said again within this long of the last time. */
		double CooldownS = 0.0;
		/** A queued line of the same group is replaced by the newer one. */
		FName Group;
	};

	inline constexpr int32 SpotterInterruptPriority = 100;
	/** The breath between two lines. */
	inline constexpr double GapS = 0.25;
	/** The finishing places with a line of their own (`position_<n>`). */
	inline constexpr int32 MaxPositionLine = 24;

	/** A side holds this long before it is called, and stays clear this long before it is called clear. */
	inline constexpr double SideOnS = 0.15;
	inline constexpr double SideOffS = 0.6;
	/** No calls of cars alongside below this speed (the grid, the pit lane, a spin). */
	inline constexpr float SpotterMinKph = 40.0f;
	/** A yellow gone for this long is called clear. */
	inline constexpr double YellowClearS = 1.5;
	/** The most worn tyre past this is called once a set; a fresh set re-arms it. */
	inline constexpr float TyreWornPct = 70.0f;
	inline constexpr float TyreFreshPct = 30.0f;

	/** Every line the triggers can say, in lines.yml order. */
	APEXSIM_API const TArray<FName>& AllLines();
	/** A line's channel, priority, wait and cool-down; an unknown id is a low engineer line. */
	APEXSIM_API FLineSpec SpecOf(FName Line);

	/** The radio's inputs from the HUD's data points. bDriving is the caller's. */
	APEXSIM_API FApexRadioInputs InputsFromHud(const FApexHudData& Data, bool bDriving);

	/**
	 * A WAV file's samples as mono floats (16-bit PCM, any rate, channels
	 * averaged). False for anything else.
	 */
	APEXSIM_API bool ParseWav(const TArray<uint8>& Bytes, TArray<float>& OutSamples, int32& OutSampleRate);
}

/**
 * Turns the inputs, frame by frame, into the lines to say. The first frame
 * after a Reset only takes stock: joining a race under a yellow says nothing.
 */
struct APEXSIM_API FApexRadioTriggers
{
	void Reset();

	/** Adds the lines to say now to Out, in order. */
	void Step(const FApexRadioInputs& In, double Now, TArray<FName>& Out);

private:
	struct FSide
	{
		/** The debounced state: what the spotter has called. */
		bool bOn = false;
		/** When the raw state last differed from bOn; negative while it agrees. */
		double ChangedAt = -1.0;
	};
	/** Moves the side towards bRaw; true when its called state flipped. */
	static bool StepSide(FSide& Side, bool bRaw, double Now);

	bool bPrimed = false;
	FSide Left;
	FSide Right;
	int32 Yellow = 0;
	/** When the yellow went out; negative while one is out or none was called. */
	double YellowGoneAt = -1.0;
	bool bBlue = false;
	FString Phase;
	bool bPitRequested = false;
	FString FuelState;
	bool bTyresCalled = false;
	int32 Lap = 0;
	bool bFinalLap = false;
	int32 FinishPosition = 0;
};

/**
 * One radio, two voices: the lines wait here and go out one at a time, the
 * most urgent first.
 */
struct APEXSIM_API FApexRadioQueue
{
	void Reset();

	/** Queue a line, unless it was said within its cool-down; it replaces a queued line of its group. */
	void Push(FName Line, double Now);

	/**
	 * The line to start now, or NAME_None: the radio is busy, the queue is
	 * empty, or only engineer lines wait while bHoldEngineer (a car alongside:
	 * no fuel talk in a fight). Lines that waited too long are dropped. A
	 * spotter call cuts a playing engineer line short: bOutInterrupt says so.
	 * The line returned counts as said; call Started with its length.
	 */
	FName Pop(double Now, bool bHoldEngineer, bool& bOutInterrupt);

	/** The line Pop returned plays for DurationS from Now. */
	void Started(FName Line, double Now, double DurationS);

	bool IsBusy(double Now) const { return Now < BusyUntil; }
	int32 Num() const { return Entries.Num(); }

private:
	struct FEntry
	{
		FName Line;
		double QueuedAt = 0.0;
		int32 Seq = 0;
	};
	TArray<FEntry> Entries;
	TMap<FName, double> LastSaid;
	FName Playing;
	double BusyUntil = 0.0;
	int32 NextSeq = 0;
};
