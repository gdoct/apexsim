#pragma once

#include "CoreMinimal.h"

/**
 * The hybrid's deployment modes, as the server's `hybrid::ErsMode` numbers
 * them (`PlayerInput.ers_mode`, bits 0-1 of telemetry's `ers_flags`).
 */
namespace ApexErs
{
	enum class EMode : int32
	{
		/** Never deploy; recover on the throttle too. */
		Harvest = 0,
		/** Deploy at full throttle, paced over the lap (the default). */
		Balanced = 1,
		/** Deploy at any throttle until the energy is gone. */
		Attack = 2,
	};

	/** The mode the ERS key steps to from `Current`: Balanced -> Attack ->
	 * Harvest -> Balanced. Anything unknown counts as Balanced. */
	inline int32 NextMode(int32 Current)
	{
		const int32 From = (Current >= 0 && Current <= 2) ? Current : static_cast<int32>(EMode::Balanced);
		return (From + 1) % 3;
	}

	/** Short HUD label: HARV, BAL, ATK; empty for no hybrid. */
	inline FString ShortLabel(int32 Mode)
	{
		switch (Mode)
		{
		case 0: return TEXT("HARV");
		case 1: return TEXT("BAL");
		case 2: return TEXT("ATK");
		default: return FString();
		}
	}

	/** The full name, for the log and the toast. */
	inline FString Name(int32 Mode)
	{
		switch (Mode)
		{
		case 0: return TEXT("Harvest");
		case 1: return TEXT("Balanced");
		case 2: return TEXT("Attack");
		default: return FString();
		}
	}
}
