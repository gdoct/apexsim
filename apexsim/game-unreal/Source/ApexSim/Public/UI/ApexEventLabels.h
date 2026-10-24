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

	/** Seconds as a countdown: 0:09, 4:59, 1:02:03, then 2D 04H past a day. */
	inline FString Countdown(int64 Seconds)
	{
		Seconds = FMath::Max<int64>(Seconds, 0);
		const int64 Days = Seconds / 86400;
		const int64 Hours = Seconds % 86400 / 3600;
		const int64 Minutes = Seconds % 3600 / 60;
		const int64 Secs = Seconds % 60;
		if (Days > 0)
		{
			return FString::Printf(TEXT("%lldD %02lldH"), Days, Hours);
		}
		return Hours > 0 ? FString::Printf(TEXT("%lld:%02lld:%02lld"), Hours, Minutes, Secs)
			: FString::Printf(TEXT("%lld:%02lld"), Minutes, Secs);
	}

	/**
	 * The lobby's line for a phase with a start time (its schedule's): it
	 * starts on time whoever is there. "THE RACE STARTS IN 4:59", then
	 * "THE RACE IS STARTING" once the time has come.
	 */
	inline FString ScheduledLine(int32 Phase, int64 StartsAt, int64 Now)
	{
		const FString PhaseText = PhaseName(Phase);
		if (PhaseText.IsEmpty() || Phase > 3 || StartsAt <= 0)
		{
			return FString();
		}
		return StartsAt > Now
			? FString::Printf(TEXT("THE %s STARTS IN %s"), *PhaseText, *Countdown(StartsAt - Now))
			: FString::Printf(TEXT("THE %s IS STARTING"), *PhaseText);
	}

	/** Where a listed event stands, for the session browser's Events list. */
	enum class EStatus : uint8
	{
		/** No session open for it yet. */
		NotOpen,
		/** Its session is open: in its lobby, or between two phases. */
		Open,
		/** A phase is being driven. */
		UnderWay,
		/** The race is run. */
		Over,
	};

	/** `Session`: the listed session open for it, or null. */
	inline EStatus Status(const FApexLobbyEvent& Event, const FApexSessionSummary* Session)
	{
		if (Event.IsDone())
		{
			return EStatus::Over;
		}
		if (!Session)
		{
			return EStatus::NotOpen;
		}
		return Session->IsWatchable() || (Session->State == EApexSessionState::Finished && !Session->bWeekendWaiting)
			? EStatus::UnderWay : EStatus::Open;
	}

	/**
	 * A listed event's badge: "In 4:59" while its phase's time is to come,
	 * else Under way, Open, Not open or Over.
	 */
	inline FString Badge(const FApexLobbyEvent& Event, const FApexSessionSummary* Session, int64 Now)
	{
		const EStatus At = Status(Event, Session);
		if (At != EStatus::Over && At != EStatus::UnderWay && Event.StartsAt > Now)
		{
			return FString::Printf(TEXT("In %s"), *Countdown(Event.StartsAt - Now));
		}
		switch (At)
		{
		case EStatus::Over:     return TEXT("Over");
		case EStatus::UnderWay: return TEXT("Under way");
		case EStatus::Open:     return TEXT("Open");
		default:                return TEXT("Not open");
		}
	}

	/** The phase a listed event is at, as its row says it: "Race next", "Practice", "Over". */
	inline FString PhaseWords(const FApexLobbyEvent& Event, const FApexSessionSummary* Session)
	{
		const FString Name = PhaseName(Event.Phase);
		if (Event.IsDone() || Name.IsEmpty())
		{
			return TEXT("Over");
		}
		const FString Phase = Name.Left(1) + Name.Mid(1).ToLower();
		return Status(Event, Session) == EStatus::UnderWay ? Phase : Phase + TEXT(" next");
	}

	/**
	 * The listed events in the order the browser shows them: the ones to
	 * come by their start time (those started by hand after them, by
	 * name), then the ones over.
	 */
	inline void SortEvents(TArray<FApexLobbyEvent>& Events)
	{
		Events.StableSort([](const FApexLobbyEvent& A, const FApexLobbyEvent& B)
		{
			if (A.IsDone() != B.IsDone())
			{
				return !A.IsDone();
			}
			const bool bTimedA = A.StartsAt > 0;
			const bool bTimedB = B.StartsAt > 0;
			if (bTimedA != bTimedB)
			{
				return bTimedA;
			}
			if (bTimedA && A.StartsAt != B.StartsAt)
			{
				return A.StartsAt < B.StartsAt;
			}
			return A.Name < B.Name;
		});
	}

	/** Milliseconds as a lap or race time: 1:21.234, 1:02:03.456 past an hour; an em dash for none. */
	inline FString Ms(int32 Ms)
	{
		if (Ms <= 0)
		{
			return TEXT("—");
		}
		const int32 Hours = Ms / 3600000;
		const int32 Minutes = Ms / 60000 % 60;
		const int32 Seconds = Ms / 1000 % 60;
		const int32 Millis = Ms % 1000;
		return Hours > 0 ? FString::Printf(TEXT("%d:%02d:%02d.%03d"), Hours, Minutes, Seconds, Millis)
			: FString::Printf(TEXT("%d:%02d.%03d"), Minutes, Seconds, Millis);
	}

	/**
	 * A sheet line's time: the race's time from the green for a finisher,
	 * its status for any other car (RETIRED, RUNNING); else the best lap,
	 * "no time" for none.
	 */
	inline FString SheetTime(const FApexEventSheetLine& Line, int32 Phase)
	{
		if (Phase == 3)
		{
			if (!Line.Status.IsEmpty() && Line.Status != TEXT("finished"))
			{
				return Line.Status.ToUpper();
			}
			if (Line.RaceTimeMs > 0)
			{
				return Ms(Line.RaceTimeMs);
			}
		}
		return Line.BestLapMs > 0 ? Ms(Line.BestLapMs) : FString(TEXT("no time"));
	}

	/**
	 * What follows a sheet line's time: its laps, "out in Q1" for a car
	 * knocked out, AI for an AI driver ("12 laps · out in Q1 · AI").
	 */
	inline FString SheetNotes(const FApexEventSheetLine& Line)
	{
		TArray<FString> Parts;
		if (Line.Laps > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d lap%s"), Line.Laps, Line.Laps == 1 ? TEXT("") : TEXT("s")));
		}
		if (Line.KnockedOutIn > 0)
		{
			Parts.Add(FString::Printf(TEXT("out in Q%d"), Line.KnockedOutIn));
		}
		if (Line.bAi)
		{
			Parts.Add(TEXT("AI"));
		}
		return FString::Join(Parts, TEXT(" · "));
	}

	/**
	 * A kept phase's header: "fastest Ada 1:21.234" for practice, "pole Bea
	 * 1:20.000" for qualifying, "won by Ada in 13:32.345" for the race.
	 */
	inline FString SheetHeadline(const FApexEventSheet& Sheet)
	{
		if (Sheet.Lines.IsEmpty())
		{
			return TEXT("nobody took part");
		}
		const FApexEventSheetLine& First = Sheet.Lines[0];
		switch (Sheet.Phase)
		{
		case 1:  return FString::Printf(TEXT("fastest %s %s"), *First.Name, *SheetTime(First, 1));
		case 2:  return FString::Printf(TEXT("pole %s %s"), *First.Name, *SheetTime(First, 2));
		default: return FString::Printf(TEXT("won by %s in %s"), *First.Name, *SheetTime(First, 3));
		}
	}

	/** A championship's header line: "2 / 3 rounds raced · leader Ada 43 pts". */
	inline FString ChampionshipLine(const FApexChampionship& Champ)
	{
		int32 Raced = 0;
		for (const FApexChampionshipRound& Round : Champ.Rounds)
		{
			Raced += Round.bRaced ? 1 : 0;
		}
		FString Line = FString::Printf(TEXT("%d / %d round%s raced"), Raced, Champ.Rounds.Num(), Champ.Rounds.Num() == 1 ? TEXT("") : TEXT("s"));
		if (Champ.Standings.Num() > 0 && Raced > 0)
		{
			Line += FString::Printf(TEXT(" · leader %s %d pts"), *Champ.Standings[0].Name, Champ.Standings[0].Points);
		}
		return Line;
	}

	/** A standing's line: "43 pts · 2 wins · R1 P1 · R2 P3 · R3 —". */
	inline FString StandingLine(const FApexChampionshipStanding& Standing)
	{
		TArray<FString> Parts;
		Parts.Add(FString::Printf(TEXT("%d pts"), Standing.Points));
		if (Standing.Wins > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d win%s"), Standing.Wins, Standing.Wins == 1 ? TEXT("") : TEXT("s")));
		}
		for (int32 Round = 0; Round < Standing.Results.Num(); ++Round)
		{
			const int32 Place = Standing.Results[Round];
			Parts.Add(Place > 0 ? FString::Printf(TEXT("R%d P%d"), Round + 1, Place) : FString::Printf(TEXT("R%d —"), Round + 1));
		}
		if (Standing.bAi)
		{
			Parts.Add(TEXT("AI"));
		}
		return FString::Join(Parts, TEXT(" · "));
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
