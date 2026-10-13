#pragma once

#include "CoreMinimal.h"

/**
 * The camera at the pit box.
 *
 * The chase camera's boom has no collision test, and a car stopped at its box
 * stands in a garage's mouth: seen from behind on the boom it was half hidden
 * by a garage column, with the garage's wall at its side. Turning the boom's
 * test on in the lane put the camera under the ground (the track's collision
 * is traces-only), and lifting the boom put it in the canopy. So while the
 * player's car stands at its box the chase view gives way to a pose of its
 * own: in the lane, at its far edge (the pit wall's, away from the garages),
 * a little ahead of the car and looking back at it, with the garage behind
 * the car rather than between it and the camera.
 *
 * Pure maths in the server's frame (metres, +Y left) so it can be tested
 * without a world; the race director turns it into a camera
 * (`AApexRaceDirector::UpdatePitCamera`).
 */
namespace ApexPitCamera
{
	/** How far inside the lane's far edge the camera stands, m. */
	inline constexpr float InsideEdgeM = 1.0f;
	/** How far ahead of the stop spot, down the lane, m. */
	inline constexpr float AheadM = 4.5f;
	/** How high over the car's origin, cm, and where on the car it looks, cm up. */
	inline constexpr float HeightCm = 170.0f;
	inline constexpr float AimHeightCm = 50.0f;
	inline constexpr float FovDeg = 58.0f;
	/** The pose holds while the car is this close to its spot, m, and this slow, m/s... */
	inline constexpr float HoldRadiusM = 4.0f;
	inline constexpr float HoldSpeedMps = 3.0f;
	/** ...and starts once it is this slow, m/s (or the crew are at work). */
	inline constexpr float StartSpeedMps = 1.5f;

	/**
	 * Where the camera stands, in the server's frame (z is the spot's own;
	 * the director sets the height off the car). `LateralM` is the spot's
	 * distance off the lane's middle, positive to the left of `YawRad`: the
	 * garage side. A spot on the middle (no side known) puts the camera to
	 * the right.
	 */
	inline FVector Position(const FVector& SpotM, float YawRad, float LateralM, float LaneWidthM)
	{
		const float GarageSide = LateralM > 0.0f ? 1.0f : -1.0f;
		const float Half = FMath::Max(LaneWidthM * 0.5f - InsideEdgeM, 0.0f);
		// From the spot to the lane's middle, then out to the far edge.
		const float Left = -LateralM - GarageSide * Half;
		const float Cos = FMath::Cos(YawRad);
		const float Sin = FMath::Sin(YawRad);
		return SpotM + FVector(AheadM * Cos - Left * Sin, AheadM * Sin + Left * Cos, 0.0f);
	}

	/**
	 * Whether the pose holds this frame: it starts when the car stands at its
	 * spot (the crew at work, or all but stopped on it) and lets go once the
	 * car pulls away.
	 */
	inline bool Holds(bool bWasHeld, bool bServicing, float DistanceM, float SpeedMps)
	{
		if (DistanceM > HoldRadiusM)
		{
			return false;
		}
		if (bServicing)
		{
			return true;
		}
		return SpeedMps < (bWasHeld ? HoldSpeedMps : StartSpeedMps);
	}
}
