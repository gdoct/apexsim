#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

/**
 * The broadcast camera: a television director for a race nobody is driving
 * from this seat, which is what the menu's demo plays behind the shell.
 *
 * It does the two jobs a real director does. It decides who to watch — a
 * battle over a lone car, the leader over the midfield, a car that has just
 * gone off over either — and it cuts between camera positions the way race
 * coverage does: a trackside operator panning a long lens as the car comes
 * past, a helicopter holding the field below, a tracking car alongside, a
 * low chase, the onboard T-cam, a nose camera, a camera on the car ahead
 * looking back; and on the grid a crane down the rows, then the pole-sitter.
 *
 * Pure logic, like ApexCockpitLayout: everything is Unreal world space
 * (cm, +Z up) and the two things only a world can answer — how high the
 * ground is, and whether something is in the way — come in as functions.
 * `ApexSim.Tv.*` automation tests drive it with cars on a synthetic circuit.
 */
namespace ApexTv
{
	enum class EShot : uint8
	{
		None,
		/** Countdown: a crane down the grid, then low on the pole-sitter. */
		Grid,
		/** A fixed camera ahead of the car, off the track, panning as it passes. */
		Trackside,
		/** High and wide, circling slowly: the car in the context of the circuit. */
		Helicopter,
		/** Alongside at the car's speed, drifting from the rear quarter to the front. */
		Tracking,
		/** Low behind on a long lens. */
		Chase,
		/** The T-cam above the driver's head. */
		Onboard,
		/** Low on the nose, looking down the road. */
		Nose,
		/** On the car ahead, looking back: the follower fills the frame. */
		Reverse,
		Count
	};

	const TCHAR* ShotName(EShot Shot);

	/** One car, as the director sees it this frame. */
	struct FCar
	{
		int32 CarIndex = -1;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		/** Measured from the car's own motion, cm/s. */
		FVector Velocity = FVector::ZeroVector;
		/** The body in the car's frame (cm, +X nose, +Z up). */
		FBox Body = FBox(FVector(-240.0, -90.0, 0.0), FVector(240.0, 90.0, 120.0));
		/** Driver's eye in the car's frame, for the T-cam. */
		FVector EyeLocal = FVector(-40.0, 0.0, 90.0);
		/** Race order: laps and centerline station together (ApexRace::RaceDistanceM). */
		float RaceDistanceM = 0.0f;
		/** Past the track limits (curbs count as track), as the server says. */
		bool bOffTrack = false;

		float SpeedCmPerS() const { return static_cast<float>(Velocity.Size2D()); }
		/** Where a camera should look: the middle of the body. */
		FVector Centre() const;
	};

	/** What a camera shows this frame. */
	struct FPose
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		/** Horizontal field of view, degrees. */
		float FovDeg = 60.0f;
		/** Distance to the subject, for depth of field. */
		float FocusDistanceCm = 0.0f;
		/** F-stop for the depth of field; 0 for none (onboard shots). */
		float Aperture = 0.0f;
	};

	/** What only the world knows. Either may be unset, which reads as "flat, clear". */
	struct FWorldQueries
	{
		/** Height of the ground below a point, cm; false if nothing was hit. */
		TFunction<bool(const FVector& Above, double& OutGroundZ)> GroundZ;
		/** Nothing but cars between two points. */
		TFunction<bool(const FVector& From, const FVector& To)> IsClear;
	};

	/**
	 * The circuit's centerline as a closed loop in Unreal space (Z is left
	 * at 0: ground height comes from the world), with cumulative stations so
	 * a camera can be put a distance down the road.
	 */
	struct APEXSIM_API FPath
	{
		TArray<FVector> Points;
		/** Station at each point, cm; the loop closes back to point 0 at LengthCm. */
		TArray<double> Stations;
		double LengthCm = 0.0;

		/** From the lobby's centerline, server frame (metres, +Y left). */
		void BuildFromServerCenterline(const TArray<FVector2D>& CenterlineMetres);
		void Build(TArray<FVector> InPoints);

		bool IsValid() const { return Points.Num() >= 3 && LengthCm > 0.0; }

		/** Station of the closest point on the loop to a location, plan view. */
		double StationAt(const FVector& Location) const;

		/** Point and unit tangent at a station, which wraps. */
		void Sample(double StationCm, FVector& OutPoint, FVector& OutTangent) const;

		/**
		 * How the road bends over a stretch either side of a station: positive
		 * towards +Y of the tangent (Unreal's right), degrees of heading change.
		 */
		double TurnDeg(double StationCm, double HalfSpanCm) const;
	};

	/** Horizontal FOV that makes a subject of this size fill a fraction of the frame's width. */
	APEXSIM_API float FramingFovDeg(float SubjectSizeCm, float DistanceCm, float FrameFraction, float MinDeg, float MaxDeg);

	/**
	 * The car most worth watching. Battles (a car within ~35 m of another),
	 * the leader and a car in trouble score; the car already on screen is
	 * kept for a few shots and then let go. Returns an index into `Cars`, or
	 * INDEX_NONE for an empty field.
	 */
	APEXSIM_API int32 PickTarget(TConstArrayView<FCar> Cars, int32 CurrentCarIndex, int32 ShotsOnCurrent,
		const TSet<int32>& CarsInTrouble, FRandomStream& Rng);

	/** Settings a caller may want to change; the defaults are the look that was tuned. */
	struct FTuning
	{
		/** Scales every duration: below 1 cuts faster. */
		float PaceScale = 1.0f;
		/** Handheld wobble on the operated cameras; 0 locks them off. */
		float Handheld = 1.0f;
	};

	class APEXSIM_API FDirector
	{
	public:
		FDirector();

		void Reset(int32 Seed);
		void SetPath(const TArray<FVector2D>& CenterlineMetres);
		const FPath& GetPath() const { return Path; }
		FTuning& Tuning() { return Settings; }

		/**
		 * Advance by a frame and say where the camera is. False when there is
		 * nothing to film (no cars).
		 *
		 * @param bCountdown the field is on the grid waiting for the lights
		 * @param TimeSeconds a steadily rising clock, for the handheld noise
		 */
		bool Tick(TConstArrayView<FCar> Cars, bool bCountdown, float DeltaSeconds, float TimeSeconds,
			const FWorldQueries& World, FPose& OutPose);

		EShot GetShot() const { return Shot; }
		int32 GetTargetCarIndex() const { return TargetCarIndex; }
		float GetShotAge() const { return ShotAge; }
		/** Cuts so far, for tests and the debug readout. */
		int32 GetCutCount() const { return CutCount; }
		/** Why the last cut happened, and how long the shot before it had been held. */
		const TCHAR* GetLastCutReason() const { return LastCutReason; }
		float GetLastShotHeld() const { return LastShotHeld; }

		/** Seconds a view may stay blocked before it is cut. */
		static constexpr float BlockedCutSeconds = 0.8f;

		/**
		 * Cut to a shot on the next tick and keep coming back to it (debugging).
		 * None lets the director choose again, from the end of the shot on screen.
		 */
		void ForceShot(EShot InShot)
		{
			ForcedShot = InShot;
			if (InShot != EShot::None)
			{
				RequestCutFor(TEXT("forced"));
			}
		}
		/** Cut on the next tick. */
		void RequestCut() { RequestCutFor(TEXT("asked")); }

	private:
		/** Where a shot's camera stands, decided at the cut. */
		struct FShotSetup
		{
			FVector Anchor = FVector::ZeroVector;
			FVector Axis = FVector::ForwardVector;
			double Side = 1.0;
			double DistanceCm = 0.0;
			double HeightCm = 0.0;
			double OrbitDeg = 0.0;
			double OrbitRateDegPerS = 0.0;
			double StartCm = 0.0;
			double EndCm = 0.0;
			float FrameFraction = 0.3f;
			int32 Variant = 0;
		};

		void RequestCutFor(const TCHAR* Reason);
		void Cut(TConstArrayView<FCar> Cars, bool bCountdown, const FWorldQueries& World);
		EShot ChooseShot(const FCar& Target, TConstArrayView<FCar> Cars, bool bCountdown);
		bool SetUpShot(EShot InShot, const FCar& Target, TConstArrayView<FCar> Cars, const FWorldQueries& World);
		void Evaluate(const FCar& Target, TConstArrayView<FCar> Cars, float DeltaSeconds, float TimeSeconds,
			const FWorldQueries& World, FPose& OutPose);
		/** True once the shot has said what it had to. */
		bool IsShotSpent(const FCar& Target, const FPose& Pose) const;
		void TrackTrouble(TConstArrayView<FCar> Cars, bool bCountdown, float DeltaSeconds);

		const FCar* FindCar(TConstArrayView<FCar> Cars, int32 CarIndex) const;
		double GroundBelow(const FVector& At, double Fallback, const FWorldQueries& World) const;

		FPath Path;
		FTuning Settings;
		FRandomStream Rng;

		EShot Shot = EShot::None;
		EShot ForcedShot = EShot::None;
		FShotSetup Setup;
		int32 TargetCarIndex = INDEX_NONE;
		int32 ShotsOnTarget = 0;
		float ShotAge = 0.0f;
		float ShotLength = 0.0f;
		float HiddenFor = 0.0f;
		int32 CutCount = 0;
		const TCHAR* PendingCutReason = TEXT("start");
		const TCHAR* LastCutReason = TEXT("start");
		float LastShotHeld = 0.0f;
		bool bCutRequested = false;
		bool bWasCountdown = false;
		/** The last few shots, newest first, so the cutting does not repeat itself. */
		TArray<EShot, TInlineAllocator<4>> History;

		/** The camera's eased state; snapped on a cut. */
		bool bFresh = true;
		FVector CamLocation = FVector::ZeroVector;
		FQuat CamRotation = FQuat::Identity;
		float CamFov = 60.0f;
		/** Heading of the frame the car-relative shots hang from, eased. */
		double FrameYawDeg = 0.0;
		FVector PrevTargetLocation = FVector::ZeroVector;
		/** The trackside camera saw the car arrive: now it waits for it to leave. */
		bool bApproached = false;

		/** Car index -> seconds it has looked in trouble (off the road, or stopped). */
		TMap<int32, float> SlowFor;
		TSet<int32> InTrouble;
		/** A car got into trouble off camera; cut to it once the current shot allows. */
		bool bTroubleUnseen = false;
		/** Seconds until an incident may interrupt the coverage again. */
		float TroubleCooldown = 0.0f;
		float RacingFor = 0.0f;
	};
}
