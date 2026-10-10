#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

struct FApexTrackGuide;

/**
 * The track guide's clock (docs/content/track-guide.md): pure, so the loop, the
 * fast-forward and the cuts are tested without a world.
 *
 * The guide owns the recording's clock outright; the race director only
 * places the cars at whatever time this says (AApexRaceDirector::
 * SetGuideClock). Four phases:
 *
 * - Overview: frozen at the overview moment (car 0 at the line).
 * - Travel:   running forward at TravelRate toward the next corner's loop;
 *             a target further than TravelRate x MaxTravelSeconds away is
 *             jumped to that far short of it first (a cut), and one closer
 *             than MinTravelSeconds of clip is simply cut to.
 * - Normal:   the corner's loop at 1x, from FromS to ToS;
 * - Slow:     then SlowFromS to SlowToS at the corner's SlowRate, then back
 *             to FromS (a cut each time), for as long as the corner is shown.
 *
 * Any backwards move (Previous, a corner behind the clock, the loop's
 * seams) is a cut: the poses are absolute, so a seek is only a new time,
 * but the cameras must be told to stop easing.
 */
namespace ApexGuide
{
	enum class EPhase : uint8
	{
		Overview,
		Travel,
		Normal,
		Slow,
	};

	APEXSIM_API const TCHAR* PhaseName(EPhase Phase);

	/** One corner's loop, from the guide file. */
	struct FLoop
	{
		double FromS = 0.0;
		double ToS = 0.0;
		double SlowFromS = 0.0;
		double SlowToS = 0.0;
		float SlowRate = 0.25f;

		bool HasSlowPart() const { return SlowToS > SlowFromS + 1.0e-3 && SlowRate > 0.0f; }
	};

	struct FTuning
	{
		/** Clip seconds per second while travelling to a corner. */
		float TravelRate = 6.0f;
		/** The longest a fast-forward runs before it is cut short. */
		float MaxTravelSeconds = 4.0f;
		/** Closer than this (in wall seconds at TravelRate) is a cut, not a fast-forward. */
		float MinTravelSeconds = 0.4f;
	};

	class APEXSIM_API FPlayer
	{
	public:
		/** The guide's corners and overview, the recording's length; shows the overview. */
		void Setup(double InOverviewS, const TArray<FLoop>& InLoops, double InDurationS);
		/** Setup from a parsed guide file. */
		void Setup(const FApexTrackGuide& Guide, double InDurationS);

		FTuning& Tuning() { return Settings; }

		/** Back to the frozen overview (a cut). */
		void ShowOverview();
		/**
		 * Show a corner (0-based). Ahead of the clock it is travelled to,
		 * unless `bCut`; behind it, or with `bCut`, it is cut to.
		 */
		void GoToCorner(int32 Index, bool bCut = false);
		/** Overview -> corner 1 -> ... -> last -> overview. */
		void Next();
		/** Last <- ... <- corner 1 <- overview; false at the overview (nowhere to go). */
		bool Previous();

		void SetPaused(bool bInPaused) { bPaused = bInPaused; }
		void TogglePause() { bPaused = !bPaused; }
		bool IsPaused() const { return bPaused; }

		/** Advance by wall seconds. */
		void Tick(double DeltaSeconds);

		double GetTime() const { return Time; }
		EPhase GetPhase() const { return Phase; }
		/** The corner shown or travelled to (0-based), INDEX_NONE at the overview. */
		int32 GetCorner() const { return Corner; }
		int32 NumCorners() const { return Loops.Num(); }
		/** Clip seconds per wall second right now: 0 paused or at the overview. */
		float GetRate() const;
		/** Times the corner's loop has come round (from 0 on arriving). */
		int32 GetLoopCount() const { return LoopCount; }

		/**
		 * True once after a jump in the clock (seek, loop seam, a travel cut
		 * short): the cameras should snap rather than ease.
		 */
		bool ConsumeCut()
		{
			const bool bWas = bCut;
			bCut = false;
			return bWas;
		}

	private:
		void CutTo(double Seconds);
		double Clamp(double Seconds) const { return FMath::Clamp(Seconds, 0.0, FMath::Max(DurationS, 0.0)); }

		FTuning Settings;
		TArray<FLoop> Loops;
		double OverviewS = 0.0;
		double DurationS = 0.0;

		EPhase Phase = EPhase::Overview;
		int32 Corner = INDEX_NONE;
		double Time = 0.0;
		bool bPaused = false;
		bool bCut = false;
		int32 LoopCount = 0;
	};

	/** What a key does in the guide. */
	enum class EAction : uint8
	{
		None,
		Previous,
		Next,
		Camera,
		CameraBack,
		Pause,
		Overview,
		Corner,
		Leave,
	};

	struct FCommand
	{
		EAction Action = EAction::None;
		/** For Corner: 1-based. */
		int32 Corner = 0;
	};

	/**
	 * The guide's keys: Left / PageUp / pad D-pad left / left shoulder back a
	 * corner, Right / PageDown / D-pad right / right shoulder on; C or pad Y
	 * the next camera (Shift+C, pad X the one before); Space, pad A or Start
	 * pause; Home the overview; 1-9 and 0 jump to corners 1-10; Escape,
	 * Backspace or pad B leave.
	 */
	APEXSIM_API FCommand CommandFor(const FKey& Key, bool bShift = false);
}
