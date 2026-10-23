#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * How the session lobby names a race event and its phase
 * (`SessionJoined.Event`; docs/game/client.md, "Session lobby"). An event
 * resumed after a server restart opens at its next phase, but its weekend
 * stands at the lobby (`WeekendState.Phase` 0) until it is started: the
 * event's phase is what the lobby says comes next, and once the weekend has
 * begun its own phase is.
 */
namespace ApexEventLabels
{
	/** The phase to show: the weekend's once it has begun, else the event's. */
	inline int32 ShownPhase(const FApexSessionEvent& Event, EApexWeekendPhase WeekendPhase)
	{
		return WeekendPhase != EApexWeekendPhase::Lobby ? static_cast<int32>(WeekendPhase) : Event.Phase;
	}

	/** A phase's name, upper case: PRACTICE, QUALIFYING, RACE; OVER past the race. */
	inline FString PhaseName(int32 Phase)
	{
		switch (Phase)
		{
		case 1:  return TEXT("PRACTICE");
		case 2:  return TEXT("QUALIFYING");
		case 3:  return TEXT("RACE");
		default: return Phase > 3 ? TEXT("OVER") : FString();
		}
	}

	/**
	 * The lobby's state line for an event: its name and the phase it is at
	 * ("AUTUMN CUP · RACE NEXT", or "· QUALIFYING" once under way).
	 */
	inline FString LobbyState(const FApexSessionEvent& Event, EApexWeekendPhase WeekendPhase)
	{
		const FString Name = Event.Name.TrimStartAndEnd().ToUpper();
		const int32 Phase = ShownPhase(Event, WeekendPhase);
		const FString PhaseText = PhaseName(Phase);
		if (PhaseText.IsEmpty())
		{
			return Name;
		}
		if (Phase > 3)
		{
			return FString::Printf(TEXT("%s · %s"), *Name, *PhaseText);
		}
		return WeekendPhase == EApexWeekendPhase::Lobby
			? FString::Printf(TEXT("%s · %s NEXT"), *Name, *PhaseText)
			: FString::Printf(TEXT("%s · %s"), *Name, *PhaseText);
	}

	/** A phase of the event already run (and kept), in this sitting or before a restart. */
	inline bool IsPhaseDone(const FApexSessionEvent& Event, EApexWeekendPhase WeekendPhase, int32 Phase)
	{
		return Event.IsEvent() && Phase < ShownPhase(Event, WeekendPhase);
	}

	/** The line under the start button while the server has the event's next phase to start. */
	inline FString WaitingLine(const FApexSessionEvent& Event, EApexWeekendPhase WeekendPhase)
	{
		const FString PhaseText = PhaseName(ShownPhase(Event, WeekendPhase));
		return PhaseText.IsEmpty() || ShownPhase(Event, WeekendPhase) > 3
			? FString(TEXT("WAITING FOR RACE CONTROL"))
			: FString::Printf(TEXT("WAITING FOR RACE CONTROL TO START THE %s"), *PhaseText);
	}
}
