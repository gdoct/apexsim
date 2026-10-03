#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/**
 * Watching a race: the choices a spectator makes (which car, which camera,
 * what the timing tower shows) and the keys that make them. Pure, so the
 * rules are tested without a world (`ApexSim.Spectate.*`); the race director
 * applies them to its cameras and the shell routes the keys
 * (UApexRootWidget::HandleWatchKey).
 */
namespace ApexSpectate
{
	/** The spectator's cameras, in the order C steps through them. */
	enum class ECamera : uint8
	{
		/** The TV director's cuts: trackside, helicopter, onboard..., on the car being watched. */
		Broadcast,
		/** The chase camera behind the car, at the player's chase distance. */
		Chase,
		/** The driver's eye, with the car's wheel and dash. */
		Onboard,
		Count,
	};

	APEXSIM_API ECamera NextCamera(ECamera Camera);
	/** "TV", "CHASE", "ONBOARD": what the HUD shows. */
	APEXSIM_API const TCHAR* CameraName(ECamera Camera);

	/** What the timing tower's right-hand column shows, in the order T steps through. */
	enum class ETowerMode : uint8
	{
		/** The gap to the car ahead. */
		Interval,
		/** The gap to the leader. */
		Gap,
		LastLap,
		BestLap,
		/** The compound, its age and its wear. */
		Tyres,
		Count,
	};

	APEXSIM_API ETowerMode NextTowerMode(ETowerMode Mode);
	/** `interval`, `gap`, `last`, `best`, `tyres`: the `spectate.tower_mode` data point. */
	APEXSIM_API const TCHAR* TowerModeKey(ETowerMode Mode);

	/** One car's place in the race, as the order needs it. */
	struct FRunner
	{
		int32 CarIndex = INDEX_NONE;
		/** Classified place once finished, else 0. */
		int32 FinishPosition = 0;
		/** Laps and station folded into one distance (ApexRace::RaceDistanceM). */
		float RaceDistanceM = 0.0f;
	};

	/**
	 * Car indices, leader first: finishers in classified order, then the
	 * furthest round; ties by car index, so two cars on the grid do not swap.
	 * The same rule as the HUD's standings.
	 */
	APEXSIM_API TArray<int32> RaceOrder(TConstArrayView<FRunner> Runners);

	/**
	 * The car `Delta` places from `Current` in `Order` (negative: towards
	 * the leader), wrapping at either end. A car not in the order steps from
	 * the leader. INDEX_NONE for an empty order.
	 */
	APEXSIM_API int32 Step(const TArray<int32>& Order, int32 Current, int32 Delta);

	/** The car in `Position` (from 1), or INDEX_NONE when nobody is. */
	APEXSIM_API int32 AtPosition(const TArray<int32>& Order, int32 Position);

	/** What a key does while watching. */
	enum class EAction : uint8
	{
		None,
		/** The car ahead in the order. */
		PreviousCar,
		/** The car behind. */
		NextCar,
		/** The car in `FCommand::Position`. */
		Position,
		Camera,
		/** The TV director picks the car (and the camera goes back to it). */
		Auto,
		Tower,
		/** Show or hide the overlay, for a clean picture. */
		Overlay,
		/** Another race (a showcase or file; nothing in a live session). */
		NextRace,
		/** Stop watching: back to the menu, or out of the session. */
		Leave,
		/** A replay's transport: pause, ten seconds either way, slower and faster. */
		PlayPause,
		SeekBack,
		SeekForward,
		Slower,
		Faster,
	};

	struct FCommand
	{
		EAction Action = EAction::None;
		/** For EAction::Position, from 1. */
		int32 Position = 0;
	};

	/**
	 * The keyboard and pad keys of the watch view: arrows / D-pad / shoulders
	 * step through the field, 1-9 and 0 pick P1-P10, C / Y the camera, A / X
	 * the auto director, T / View the tower, H / right stick the overlay,
	 * N / left stick the next race, Backspace / B leave; a replay's transport
	 * on Space / A (pause), comma and full stop / the triggers (ten seconds
	 * back and on) and - / = (slower, faster). The pause key is not here: it
	 * opens the pause menu as in a race.
	 */
	APEXSIM_API FCommand CommandFor(const FKey& Key);

	/** The replay speeds - and = step through: 0.25, 0.5, 1, 2, 4. */
	APEXSIM_API float StepPlaybackRate(float Rate, int32 Direction);
}
