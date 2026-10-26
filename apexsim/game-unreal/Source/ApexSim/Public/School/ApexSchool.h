#pragma once

#include "CoreMinimal.h"

#include "ApexSchool.generated.h"

struct FApexTrackGuide;

/** A licence test's medal. The order is the ranking: a higher value is a better medal. */
UENUM()
enum class EApexMedal : uint8
{
	None   = 0,
	Bronze = 1,
	Silver = 2,
	Gold   = 3,
};

/** What a licence test asks (ApexSchool::FTest). */
UENUM()
enum class EApexSchoolKind : uint8
{
	/** Over a speed at the board, then stop with the car's station on the line. */
	Brake      = 0,
	/** Through a corner, judged by the speed it is left at. */
	CornerExit = 1,
	/** Through a corner, entry to exit, judged by the time. */
	CornerTime = 2,
	/** One lap inside track limits, judged by the server's lap time. */
	Lap        = 3,
};

/** A driver's best at one test with one car on one circuit, kept in the profile (UApexProfileSave::SchoolMedals). */
USTRUCT()
struct APEXSIM_API FApexSchoolMedal
{
	GENERATED_BODY()

	/** `<track id>|<car id>|<test id>` (ApexSchool::MedalKey). */
	UPROPERTY()
	FString Key;

	UPROPERTY()
	EApexMedal Medal = EApexMedal::None;

	/**
	 * The best passed try's figure in the test's own unit (ApexSchool::FResult::Value):
	 * metres off the line (signed), m/s out of the corner, or seconds. Meaningless
	 * while Medal is None.
	 */
	UPROPERTY()
	float Best = 0.0f;

	/** Tries judged, passed or not. */
	UPROPERTY()
	int32 Tries = 0;
};

/**
 * The driving school (docs/game/client.md, "Driving school"): short licence
 * tests made from a circuit's track guide (Guide/ApexTrackGuide.h), judged on
 * the client from the local car's telemetry in a hotlap session it creates
 * for itself. Pure: no world, no subsystems.
 *
 * Every station is metres from the line along the centerline, as the guide
 * and telemetry's TrackProgress both measure it.
 */
namespace ApexSchool
{
	/** What a medal asks, by fraction of the guide car's figure (the medal thresholds; tuned by play-testing). */
	namespace Rules
	{
		/** Brake: at the board at least this share of the guide car's speed there. */
		inline constexpr float BoardSpeedShare = 0.9f;
		/** Brake: the stop line sits this much further than the guide car's own deceleration would stop it. */
		inline constexpr float StopSlack = 1.1f;
		/** Brake: metres off the line, bronze, silver, gold. */
		inline constexpr float StopToleranceM[3] = { 15.0f, 8.0f, 3.0f };
		/** Corner exit: the share of the guide car's exit speed, bronze, silver, gold. */
		inline constexpr float ExitShare[3] = { 0.85f, 0.93f, 0.98f };
		/** Corner time: the guide car's time through it times this, bronze, silver, gold. */
		inline constexpr float CornerTimeShare[3] = { 1.15f, 1.07f, 1.02f };
		/** Lap: the guide car's lap time times this, bronze, silver, gold. */
		inline constexpr float LapTimeShare[3] = { 1.10f, 1.05f, 1.02f };
		/** Below this the car counts as stopped, m/s. */
		inline constexpr float StoppedMps = 0.5f;
		/** A station change this large between two samples is a relocation, not driving, metres. */
		inline constexpr float JumpM = 150.0f;
		/** The time limit: the guide car's time from the line to the test's end (pro rata of its lap) times this, ... */
		inline constexpr float TimeLimitShare = 1.5f;
		/** ... plus this, seconds (a stop, a slow start from the run-up). */
		inline constexpr float TimeLimitExtraS = 10.0f;
	}

	/** One test of a circuit. */
	struct APEXSIM_API FTest
	{
		/** "brake", "exit", "corner", "lap": the medal key's last part. */
		FString Id;
		EApexSchoolKind Kind = EApexSchoolKind::Lap;
		/** "Brake test", "Corner exit", "Corner time", "Clean lap". */
		FString Title;
		/** The corner's display name ("Turn 3"); empty for the lap. */
		FString Place;
		/** Brake: the board and the stop line. Corners: entry and exit. Unused for the lap. */
		float FromM = 0.0f;
		float ToM = 0.0f;
		/** Brake: the least speed at the board, m/s. */
		float MinSpeedMps = 0.0f;
		/**
		 * The guide car's figure the medals are cut from: its speed at the board
		 * (brake), out of the corner (exit, m/s), its time through the corner or
		 * round the lap (seconds). 0 for a corner time not measured yet.
		 */
		float Reference = 0.0f;
		/** What each medal asks, bronze, silver, gold, in the result's unit (FResult::Value); see Rules. */
		float Limits[3] = { 0.0f, 0.0f, 0.0f };
		/** Seconds from crossing the line to finish the test, or the try fails; 0 for none. */
		float TimeLimitS = 0.0f;

		bool IsReady() const { return Reference > 0.0f; }
	};

	/** How a try ended. */
	struct APEXSIM_API FResult
	{
		bool bPassed = false;
		EApexMedal Medal = EApexMedal::None;
		/**
		 * The figure judged: brake, metres past the line at the stop (negative
		 * short of it); exit, m/s out of the corner; corner and lap, seconds.
		 */
		float Value = 0.0f;
		/** Brake: the speed at the board; corners: the exit speed; m/s. */
		float SpeedMps = 0.0f;
		/** Corners: seconds entry to exit. */
		float Seconds = 0.0f;
		/** A failed try's reason, player-facing ("Too slow at the board"); empty when passed. */
		FString Why;
	};

	/** The guide's tests: a brake test, a corner exit, a corner time and a clean lap, where the guide has the corners for them. */
	APEXSIM_API TArray<FTest> BuildTests(const FApexTrackGuide& Guide);

	/** The reference (see FTest::Reference) and the medal limits from it. */
	APEXSIM_API void SetReference(FTest& Test, float Reference);

	/** The medal a figure earns; None when it misses bronze. Brake compares the distance off the line, either side. */
	APEXSIM_API EApexMedal MedalFor(const FTest& Test, float Value);

	/** A is a better figure than B at this test (both passed). */
	APEXSIM_API bool IsBetter(const FTest& Test, float A, float B);

	/** The profile's key of a test with a car on a circuit. */
	APEXSIM_API FString MedalKey(const FString& TrackId, const FString& CarId, const FString& TestId);

	/** The entry for Key, or null. */
	APEXSIM_API const FApexSchoolMedal* FindMedal(const TArray<FApexSchoolMedal>& Medals, const FString& Key);

	/**
	 * Count a judged try into the profile's medals: the try is counted, and a
	 * passed one replaces the best when its medal is higher or its figure better
	 * at the same medal. True when it did (a new best).
	 */
	APEXSIM_API bool FileResult(TArray<FApexSchoolMedal>& Medals, const FString& Key, const FTest& Test, const FResult& Result);

	// --- What the cards say (pure, for the tests) ------------------------------

	/** "GOLD", "SILVER", "BRONZE", "NO MEDAL". */
	APEXSIM_API FString MedalName(EApexMedal Medal);
	/** The colour a medal is written in. */
	APEXSIM_API FLinearColor MedalColour(EApexMedal Medal);
	/** What the test asks, one sentence. */
	APEXSIM_API FString GoalText(const FTest& Test, bool bMetric);
	/** What a medal asks: "within 3 m of the line", "142 km/h or more", "under 6.412 s". */
	APEXSIM_API FString TargetText(const FTest& Test, EApexMedal Medal, bool bMetric);
	/** A figure in the test's terms: "1.8 m past the line", "151 km/h", "6.412 s", "1:38.120". */
	APEXSIM_API FString FigureText(const FTest& Test, float Value, bool bMetric);

	/**
	 * Where a station lies on the track's centerline as built (centimetres, a
	 * closed loop from its first point): the point and the forward direction
	 * there (flat). Stations are measured in plan from the first point, as
	 * the server measures progress; past the loop's length they wrap. False
	 * for a line of fewer than two points.
	 */
	APEXSIM_API bool PoseAtStation(const TArray<FVector>& LineCm, float StationM, FVector& OutCm, FVector& OutForward);

	/** One telemetry sample of the local car, as the judge reads it. */
	struct APEXSIM_API FSample
	{
		/** A clock that only runs forward while the session does: the server tick over its rate. */
		double TimeS = 0.0;
		float StationM = 0.0f;
		float SpeedMps = 0.0f;
		int32 Lap = 0;
		bool bLapInvalid = false;
		bool bInGarage = false;
		int32 LastLapMs = 0;
		bool bLastLapInvalid = false;
	};

	/** Where a try is. */
	enum class EJudgePhase : uint8
	{
		/** Driving to the test's start (the board, the corner's entry, the line). */
		Waiting,
		/** Inside the test. */
		Running,
		/** Judged: GetResult. */
		Done,
		/** Given up (the car went back to the garage, was moved, the clock went back); not judged. */
		Aborted,
	};

	/** Judges one try of a test from the local car's samples, in order. */
	class APEXSIM_API FJudge
	{
	public:
		void Begin(const FTest& InTest, float InLapM);
		/** Take one sample; true when it ended the try (Done or Aborted). */
		bool Feed(const FSample& Sample);

		EJudgePhase GetPhase() const { return Phase; }
		const FResult& GetResult() const { return Result; }
		const FTest& GetTest() const { return Test; }
		/** Why it was aborted; empty otherwise. */
		const FString& GetAbortReason() const { return AbortReason; }
		bool IsOver() const { return Phase == EJudgePhase::Done || Phase == EJudgePhase::Aborted; }
		/** Whether the time limit's clock runs: the car has crossed the line. */
		bool IsClockRunning() const { return bClock; }
		/** Seconds of the time limit left at the last sample; the whole limit until the line is crossed. */
		float GetTimeLeftS() const;

	private:
		/** Whether the car passed station AtM between the last sample and this one; Alpha is where between. */
		bool Crossed(float AtM, float FromStation, float ToStation, float& OutAlpha) const;
		void Finish(bool bPassed, float Value, const FString& Why);
		void Abort(const FString& Why);

		FTest Test;
		float LapM = 0.0f;
		EJudgePhase Phase = EJudgePhase::Waiting;
		FResult Result;
		FString AbortReason;

		bool bHasPrev = false;
		FSample Prev;
		/** Running: when it started, the lap it started on, how far it has gone. */
		double StartTimeS = 0.0;
		int32 StartLap = 0;
		float TravelledM = 0.0f;
		bool bStruckAtStart = false;
		/** The time limit's clock: started at the line. */
		bool bClock = false;
		double ClockStartS = 0.0;
	};
}
