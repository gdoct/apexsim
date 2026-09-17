#pragma once

#include "CoreMinimal.h"

/**
 * The chase camera's distance ladder.
 *
 * There used to be two views — the cockpit and one chase camera a long way
 * back — and C flipped between them, which left nothing between sitting in
 * the car and watching it from across the road. The chase camera is now a
 * ladder of presets from a roof cam to that original distance, and C steps
 * down it: cockpit, then each rung in turn, then back to the cockpit.
 *
 * Pure data and pure maths so the ladder can be tested without a world;
 * the race director applies a rung to its spring arm in `ApplyChaseView`.
 */
namespace ApexChase
{
	/** One rung: where the boom puts the camera and how tightly it follows. */
	struct FView
	{
		/** For the log and the -ApexChaseLevel= switch. */
		const TCHAR* Name = TEXT("chase");
		/** Boom length behind the car's origin, cm. */
		float ArmLengthCm = 900.0f;
		/** How far the boom's origin is lifted off the car's own origin, cm. */
		float HeightCm = 0.0f;
		/** Boom pitch; negative looks down at the car. */
		float PitchDeg = -12.0f;
		/**
		 * Added to the chase field of view. A camera this close wants a wider
		 * lens or the car fills the screen and the corner ahead is gone.
		 */
		float FovDeltaDeg = 0.0f;
		/** Spring-arm lag. The roof cam is all but welded on; the far one drifts. */
		float LagSpeed = 6.0f;
		float RotationLagSpeed = 5.0f;
		/**
		 * How far the lag may ever leave the camera behind the boom, cm; 0 is
		 * the spring arm's own unclamped lag. Lag at a steady speed is roughly
		 * speed / LagSpeed, which at 320 km/h is nine metres — enough to turn
		 * every close rung back into the far one. The clamp is what keeps a
		 * 3 m camera at 3 m on the straight while still easing into corners.
		 */
		float LagMaxDistanceCm = 0.0f;
	};

	/**
	 * Closest first, so stepping up the index is stepping away from the car.
	 * The last rung is the chase camera as it was before the ladder existed.
	 */
	inline const TArray<FView>& Views()
	{
		static const TArray<FView> Ladder = {
			// Just over the cockpit: the roofline in shot, near enough to the
			// driver's own eyeline to place the car by, without the bodywork.
			{TEXT("roof"), 60.0f, 140.0f, -2.0f, 12.0f, 18.0f, 16.0f, 25.0f},
			// Right behind the bodywork, and just over it: at wing height the
			// road ahead is behind the rear wing (checked on the 911's).
			{TEXT("close"), 300.0f, 125.0f, -7.0f, 8.0f, 10.0f, 9.0f, 90.0f},
			// A little farther: the whole car with some road around it.
			{TEXT("near"), 560.0f, 45.0f, -9.0f, 4.0f, 8.0f, 7.0f, 180.0f},
			// The original chase camera, its lag unclamped as it always was.
			{TEXT("far"), 900.0f, 0.0f, -12.0f, 0.0f, 6.0f, 5.0f, 0.0f},
		};
		return Ladder;
	}

	inline int32 Num() { return Views().Num(); }

	/** The farthest rung: what `-ApexView=chase` and an unset setting mean. */
	inline int32 DefaultLevel() { return Num() - 1; }

	/** Any index, clamped onto the ladder. */
	inline const FView& Get(int32 Level)
	{
		return Views()[FMath::Clamp(Level, 0, Num() - 1)];
	}

	/** Level -1 is the cockpit, which is not a rung but is part of the cycle. */
	static constexpr int32 CockpitLevel = -1;

	/**
	 * What C does: cockpit -> closest -> ... -> farthest -> cockpit. An index
	 * from an older save that is off the end lands back in the cockpit rather
	 * than sticking.
	 */
	inline int32 NextLevel(int32 Level)
	{
		if (Level < 0 || Level >= Num() - 1)
		{
			return Level < 0 ? 0 : CockpitLevel;
		}
		return Level + 1;
	}

	/** "roof", "far", … -> its index; -1 for "cockpit"; nothing for anything else. */
	inline bool FindByName(const FString& Name, int32& OutLevel)
	{
		if (Name.Equals(TEXT("cockpit"), ESearchCase::IgnoreCase))
		{
			OutLevel = CockpitLevel;
			return true;
		}
		for (int32 Index = 0; Index < Num(); ++Index)
		{
			if (Name.Equals(Views()[Index].Name, ESearchCase::IgnoreCase))
			{
				OutLevel = Index;
				return true;
			}
		}
		return false;
	}
}
