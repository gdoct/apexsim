#pragma once

#include "CoreMinimal.h"
#include "Race/ApexTvDirector.h"

/**
 * How a replay clip is filmed (`-ApexReplayCam=` and friends): pure data and
 * maths, so the race director only has to place a camera where this says.
 *
 * - Tv:      the broadcast director cuts as it likes (optionally locked on
 *            one car, optionally held on one kind of shot).
 * - Fixed:   a tripod: the shot camera at `-ApexCamera=` / `-ApexCameraLookAt=`,
 *            never moving. The cars drive through the frame.
 * - Pan:     a tripod that turns to follow a car, optionally biased toward a
 *            fixed point (keep the ferris wheel in the frame), optionally
 *            zooming to hold the car at a constant size.
 * - Chase:   the chase camera on one car, at a rung of the chase ladder.
 * - Cockpit: the driver's eye in one car.
 */
namespace ApexReplayCam
{
	enum class EMode : uint8
	{
		Tv,
		Fixed,
		Pan,
		Chase,
		Cockpit,
	};

	/** Who the camera watches. */
	enum class EFollow : uint8
	{
		/** A car index given outright. */
		Index,
		/** Whoever leads the race at the start of the clip. */
		Leader,
		/** Whoever is nearest the look point (or the eye), re-picked as the field goes by. */
		Nearest,
	};

	struct FSettings
	{
		EMode Mode = EMode::Tv;
		EFollow Follow = EFollow::Leader;
		int32 FollowIndex = INDEX_NONE;
		/** Tv: keep every shot on the followed car. */
		bool bLockTv = false;
		/** Tv: hold this kind of shot (None cuts freely). */
		ApexTv::EShot TvShot = ApexTv::EShot::None;
		/** Tv: the director's random seed, so a render repeats. */
		int32 Seed = 1;

		/** Fixed/Pan: where the camera stands and (Fixed) what it looks at, Unreal cm. */
		bool bHasEye = false;
		FVector EyeCm = FVector::ZeroVector;
		bool bHasLook = false;
		FVector LookCm = FVector::ZeroVector;
		/** Fixed: the rotation an `-ApexCamera=` pose gave (when there is no look point). */
		FRotator EyeRotation = FRotator::ZeroRotator;
		/** Pan: 0 looks straight at the car, 1 straight at the look point. */
		float LookBias = 0.0f;
		/** Pan: how quickly the camera turns after the car, 1/s. */
		float PanSpeed = 5.0f;
		/** Pan: seconds of the car's velocity to lead it by. */
		float LeadSeconds = 0.2f;
		/** Fixed/Pan: horizontal field of view; with framing, the widest the lens goes. */
		float FovDeg = 50.0f;
		/** Pan: hold this many metres of width across the frame at the car (0: no zoom). */
		float FrameWidthM = 0.0f;
		/** Pan: the longest lens the zoom may reach. */
		float MinFovDeg = 3.0f;
		/** Chase: the chase ladder's rung (ApexChase), -1 for the player's setting. */
		int32 ChaseLevel = -1;
		/**
		 * Fixed/Pan: a tripod below the rendered ground plus this is lifted
		 * to it once the level is in (the pose came from the road's height,
		 * which a hillside beside it need not share). Negative: never.
		 */
		float GroundClearanceCm = 120.0f;
	};

	inline bool ParseMode(const FString& Text, EMode& Out)
	{
		static const TCHAR* Names[] = { TEXT("tv"), TEXT("fixed"), TEXT("pan"), TEXT("chase"), TEXT("cockpit") };
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
		{
			if (Text.Equals(Names[Index], ESearchCase::IgnoreCase))
			{
				Out = static_cast<EMode>(Index);
				return true;
			}
		}
		if (Text.Equals(TEXT("onboard"), ESearchCase::IgnoreCase))
		{
			Out = EMode::Cockpit;
			return true;
		}
		return false;
	}

	inline const TCHAR* ModeName(EMode Mode)
	{
		switch (Mode)
		{
		case EMode::Tv: return TEXT("tv");
		case EMode::Fixed: return TEXT("fixed");
		case EMode::Pan: return TEXT("pan");
		case EMode::Chase: return TEXT("chase");
		case EMode::Cockpit: return TEXT("cockpit");
		}
		return TEXT("?");
	}

	/** A TV shot by the name `apexsim.tv.Shot` takes. */
	inline bool ParseTvShot(const FString& Text, ApexTv::EShot& Out)
	{
		for (int32 Index = 0; Index < static_cast<int32>(ApexTv::EShot::Count); ++Index)
		{
			if (Text.Equals(ApexTv::ShotName(static_cast<ApexTv::EShot>(Index)), ESearchCase::IgnoreCase))
			{
				Out = static_cast<ApexTv::EShot>(Index);
				return true;
			}
		}
		return false;
	}

	/** "leader", "nearest" or a car index. */
	inline bool ParseFollow(const FString& Text, EFollow& OutFollow, int32& OutIndex)
	{
		if (Text.Equals(TEXT("leader"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("lead"), ESearchCase::IgnoreCase))
		{
			OutFollow = EFollow::Leader;
			return true;
		}
		if (Text.Equals(TEXT("nearest"), ESearchCase::IgnoreCase))
		{
			OutFollow = EFollow::Nearest;
			return true;
		}
		if (Text.IsNumeric())
		{
			OutFollow = EFollow::Index;
			OutIndex = FCString::Atoi(*Text);
			return OutIndex >= 0;
		}
		return false;
	}

	/**
	 * Where a pan camera wants to look: at the car, led by its velocity, and
	 * turned `LookBias` of the way toward a fixed point when there is one.
	 * Directions are blended, not points, so a far landmark and a near car
	 * mix by angle the way an operator would compose them.
	 */
	inline FRotator PanRotation(const FVector& EyeCm, const FVector& TargetCm, const FVector& TargetVelocityCmS,
		const FVector* LookCm, float LookBias, float LeadSeconds)
	{
		const FVector Led = TargetCm + TargetVelocityCmS * LeadSeconds;
		FVector Direction = (Led - EyeCm).GetSafeNormal();
		if (LookCm && LookBias > 0.0f)
		{
			const FVector ToLook = (*LookCm - EyeCm).GetSafeNormal();
			const FQuat A = Direction.ToOrientationQuat();
			const FQuat B = ToLook.ToOrientationQuat();
			Direction = FQuat::Slerp(A, B, FMath::Clamp(LookBias, 0.0f, 1.0f)).GetForwardVector();
		}
		FRotator Out = Direction.Rotation();
		Out.Roll = 0.0f;
		return Out;
	}

	/**
	 * The horizontal field of view that shows `WidthM` metres across the
	 * frame at `DistanceCm`, between the longest lens and the widest.
	 */
	inline float FramingFov(float WidthM, double DistanceCm, float MinFovDeg, float MaxFovDeg)
	{
		if (WidthM <= 0.0f || DistanceCm <= 1.0)
		{
			return MaxFovDeg;
		}
		const double Half = FMath::Atan((WidthM * 100.0 * 0.5) / DistanceCm);
		return FMath::Clamp(static_cast<float>(FMath::RadiansToDegrees(2.0 * Half)), MinFovDeg, MaxFovDeg);
	}

	/** Frame-rate independent easing: how far to move toward a target this step. */
	inline float EaseAlpha(float Rate, float DeltaSeconds)
	{
		return Rate <= 0.0f ? 1.0f : 1.0f - FMath::Exp(-Rate * FMath::Max(DeltaSeconds, 0.0f));
	}
}
