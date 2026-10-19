#pragma once

#include "CoreMinimal.h"
#include "Race/ApexTvDirector.h"

/**
 * The camera after the local car takes the chequered flag: it pulls straight
 * back and up from wherever the driving camera was (the zoom out), then
 * climbs into a slow, high orbit round the car (the panorama), which it
 * holds while the server's cool-down driver takes the car round.
 *
 * Two phases, one continuous move. While zooming out the camera stays behind
 * the car, following its (smoothed) heading; while rising into the panorama
 * the bearing is handed over to a free orbit in the world frame, so the
 * circuit turns past behind the car instead of the camera swinging with every
 * corner.
 *
 * Pure logic, like ApexTvDirector: Unreal world space (cm, +Z up), the ground
 * query passed in. `ApexSim.FinishCam.*` drives it on flat ground.
 */
namespace ApexFinishCam
{
	struct FTuning
	{
		/** The pull back behind the car. */
		float ZoomSeconds = 3.0f;
		double ZoomDistanceCm = 2200.0;
		double ZoomHeightCm = 800.0;
		float ZoomFovDeg = 60.0f;

		/** The climb into the orbit. */
		float PanoramaSeconds = 5.0f;
		double OrbitDistanceCm = 5200.0;
		double OrbitHeightCm = 2600.0;
		float PanoramaFovDeg = 55.0f;
		float OrbitDegPerSecond = 6.0f;

		/** Never closer to the ground below than this. */
		double GroundClearanceCm = 300.0;
		/** A camera further than this from the car (another car's, a broadcast shot) starts from behind it instead. */
		double MaxStartDistanceCm = 10000.0;
		/** How long the turn from the driving camera's own aim onto the car takes. */
		float AimSeconds = 0.8f;
	};

	enum class EPhase : uint8
	{
		ZoomOut,
		ToPanorama,
		Panorama,
	};

	class APEXSIM_API FMove
	{
	public:
		/**
		 * Begin from the camera on screen, filming the car at `CarLocation`.
		 * A camera too far from the car to pull back from (or none, a zero
		 * FOV) starts from a chase position behind it.
		 */
		void Start(const FVector& EyeLocation, const FRotator& EyeRotation, float FovDeg,
			const FVector& CarLocation, const FRotator& CarRotation);

		bool IsStarted() const { return bStarted; }

		/** Advance and place the camera on the car where it is now. */
		ApexTv::FPose Tick(const FVector& CarLocation, const FRotator& CarRotation, float DeltaSeconds,
			const ApexTv::FWorldQueries& World);

		EPhase GetPhase() const;
		float GetAge() const { return Age; }
		FTuning& Tuning() { return Settings; }

	private:
		FTuning Settings;
		bool bStarted = false;
		float Age = 0.0f;

		/** Where the camera began, about the car: bearing off its nose (degrees), distance and height (cm). */
		double StartBearingDeg = 180.0;
		double StartDistanceCm = 600.0;
		double StartHeightCm = 200.0;
		float StartFovDeg = 70.0f;
		FRotator StartRotation = FRotator::ZeroRotator;

		/** The car's heading, eased so a flick of the tail does not swing the camera. */
		double HeadingDeg = 0.0;
		/** The free orbit's bearing in the world frame, degrees; seeded when the climb begins. */
		double OrbitBearingDeg = 0.0;
		bool bOrbitSeeded = false;
		/** Extra height to clear rising ground, eased. */
		double LiftCm = 0.0;
	};
}
