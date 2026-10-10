#include "Race/ApexFinishCamera.h"

namespace ApexFinishCam
{
	namespace
	{
		/** 0..1 with no jolt at either end. */
		double Ease(double T)
		{
			const double X = FMath::Clamp(T, 0.0, 1.0);
			return X * X * (3.0 - 2.0 * X);
		}

		double LerpAngleDeg(double From, double To, double Alpha)
		{
			return From + FMath::FindDeltaAngleDegrees(From, To) * Alpha;
		}

		/** A chase camera's place: low behind the car, looking at it. */
		constexpr double FallbackDistanceCm = 600.0;
		constexpr double FallbackHeightCm = 200.0;
		constexpr float FallbackFovDeg = 70.0f;
		/** What the camera aims at: about the car's roof, not its floor. */
		constexpr double LookAboveCarCm = 100.0;
		/** How quickly the eased heading follows the car's, per second. */
		constexpr double HeadingRate = 2.0;
	}

	void FMove::Start(const FVector& EyeLocation, const FRotator& EyeRotation, float FovDeg,
		const FVector& CarLocation, const FRotator& CarRotation)
	{
		bStarted = true;
		Age = 0.0f;
		bOrbitSeeded = false;
		LiftCm = 0.0;
		HeadingDeg = CarRotation.Yaw;

		const FVector Rel = EyeLocation - CarLocation;
		if (FovDeg <= 0.0f || Rel.Size() > Settings.MaxStartDistanceCm)
		{
			StartBearingDeg = 180.0;
			StartDistanceCm = FallbackDistanceCm;
			StartHeightCm = FallbackHeightCm;
			StartFovDeg = FallbackFovDeg;
			const FVector Back = CarRotation.RotateVector(FVector(-FallbackDistanceCm, 0.0, FallbackHeightCm));
			StartRotation = (CarLocation + FVector(0.0, 0.0, LookAboveCarCm) - (CarLocation + Back)).Rotation();
			return;
		}

		// From the cockpit the eye is all but on the car's centre: call that behind it.
		StartBearingDeg = Rel.Size2D() < 50.0
			? 180.0
			: FMath::FindDeltaAngleDegrees(CarRotation.Yaw, FMath::RadiansToDegrees(FMath::Atan2(Rel.Y, Rel.X)));
		StartDistanceCm = Rel.Size2D();
		StartHeightCm = Rel.Z;
		StartFovDeg = FovDeg;
		StartRotation = EyeRotation;
	}

	EPhase FMove::GetPhase() const
	{
		if (Age < Settings.ZoomSeconds)
		{
			return EPhase::ZoomOut;
		}
		return Age < Settings.ZoomSeconds + Settings.PanoramaSeconds ? EPhase::ToPanorama : EPhase::Panorama;
	}

	ApexTv::FPose FMove::Tick(const FVector& CarLocation, const FRotator& CarRotation, float DeltaSeconds,
		const ApexTv::FWorldQueries& World)
	{
		if (!bStarted)
		{
			Start(FVector::ZeroVector, FRotator::ZeroRotator, 0.0f, CarLocation, CarRotation);
		}
		const double Dt = FMath::Max(0.0f, DeltaSeconds);
		Age += static_cast<float>(Dt);
		HeadingDeg = LerpAngleDeg(HeadingDeg, CarRotation.Yaw, 1.0 - FMath::Exp(-HeadingRate * Dt));

		// Behind the car, from wherever the driving camera was.
		const double Zoom = Ease(Age / FMath::Max(0.01f, Settings.ZoomSeconds));
		const double FollowBearing = HeadingDeg + LerpAngleDeg(StartBearingDeg, 180.0, Zoom);
		double Bearing = FollowBearing;
		double Distance = FMath::Lerp(StartDistanceCm, Settings.ZoomDistanceCm, Zoom);
		double Height = FMath::Lerp(StartHeightCm, Settings.ZoomHeightCm, Zoom);
		float Fov = FMath::Lerp(StartFovDeg, Settings.ZoomFovDeg, static_cast<float>(Zoom));

		if (Age >= Settings.ZoomSeconds)
		{
			// Up and out, the bearing handed from the car's heading to a free
			// orbit that starts exactly where the camera is.
			const double Climb = Ease((Age - Settings.ZoomSeconds) / FMath::Max(0.01f, Settings.PanoramaSeconds));
			if (!bOrbitSeeded)
			{
				OrbitBearingDeg = FollowBearing;
				bOrbitSeeded = true;
			}
			OrbitBearingDeg = FMath::UnwindDegrees(OrbitBearingDeg + Settings.OrbitDegPerSecond * Climb * Dt);
			Bearing = LerpAngleDeg(FollowBearing, OrbitBearingDeg, Climb);
			Distance = FMath::Lerp(Settings.ZoomDistanceCm, Settings.OrbitDistanceCm, Climb);
			Height = FMath::Lerp(Settings.ZoomHeightCm, Settings.OrbitHeightCm, Climb);
			Fov = FMath::Lerp(Settings.ZoomFovDeg, Settings.PanoramaFovDeg, static_cast<float>(Climb));
		}

		const double BearingRad = FMath::DegreesToRadians(Bearing);
		FVector Eye = CarLocation + FVector(FMath::Cos(BearingRad) * Distance, FMath::Sin(BearingRad) * Distance, Height);

		// Clear of a hillside or a grandstand roof between the camera and the sky.
		double Ground = 0.0;
		double WantLift = 0.0;
		if (World.GroundZ && World.GroundZ(Eye + FVector(0.0, 0.0, 50000.0), Ground))
		{
			WantLift = FMath::Max(0.0, Ground + Settings.GroundClearanceCm - Eye.Z);
		}
		LiftCm = FMath::FInterpTo(LiftCm, WantLift, static_cast<float>(Dt), WantLift > LiftCm ? 6.0f : 1.5f);
		LiftCm = FMath::Max(LiftCm, WantLift - Settings.GroundClearanceCm);
		Eye.Z += LiftCm;

		const FVector Look = CarLocation + FVector(0.0, 0.0, LookAboveCarCm);
		const FRotator Aim = (Look - Eye).Rotation();
		const double AimAlpha = Ease(Age / FMath::Max(0.01f, Settings.AimSeconds));
		FRotator Rotation = FQuat::Slerp(StartRotation.Quaternion(), Aim.Quaternion(), AimAlpha).Rotator();
		Rotation.Roll = 0.0;

		ApexTv::FPose Pose;
		Pose.Location = Eye;
		Pose.Rotation = Rotation;
		Pose.FovDeg = Fov;
		Pose.FocusDistanceCm = static_cast<float>(FVector::Dist(Look, Eye));
		Pose.Aperture = 0.0f;
		return Pose;
	}
}
