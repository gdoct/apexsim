#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * The race's incident list as the results screen shows it (`Incidents`,
 * docs/server/sessions.md "Incidents"; docs/game/client.md "Session
 * results"): each incident's words, and its second in the client's own
 * replay of the session, where a jump into the replay puts the watch view.
 */
namespace ApexIncidentList
{
	/** A jump into the replay starts this long before the incident, to see it coming. */
	inline constexpr double LeadSeconds = 3.0;

	/** What happened: "Contact", "Pass under yellow", "Track limits", "Penalty", "Penalty cleared". */
	inline FString What(const FApexIncident& Incident)
	{
		switch (Incident.Kind)
		{
		case ApexIncident::Contact:        return TEXT("Contact");
		case ApexIncident::YellowPass:     return TEXT("Pass under yellow");
		case ApexIncident::TrackLimits:    return TEXT("Track limits");
		case ApexIncident::Penalty:        return TEXT("Penalty");
		case ApexIncident::PenaltyCleared: return TEXT("Penalty cleared");
		default:                           return TEXT("Incident");
		}
	}

	/** Who: "Ada and Bea" for a contact, "Ada passed Bea" under a yellow, else the car's driver. */
	inline FString Cars(const FApexIncident& Incident)
	{
		const FString Name = Incident.Name.IsEmpty() ? FString(TEXT("Driver")) : Incident.Name;
		if (Incident.OtherName.IsEmpty())
		{
			return Name;
		}
		return Incident.Kind == ApexIncident::YellowPass
			? FString::Printf(TEXT("%s passed %s"), *Name, *Incident.OtherName)
			: FString::Printf(TEXT("%s and %s"), *Name, *Incident.OtherName);
	}

	/** A penalty's offence in words. */
	inline FString Offence(int32 Offence)
	{
		return Offence == ApexPenalty::JumpStart ? TEXT("jump start")
			: Offence == ApexPenalty::YellowPass ? TEXT("overtaking under yellow")
			: Offence == ApexPenalty::PitExitRed ? TEXT("pit exit on red")
			: TEXT("race control");
	}

	/** How a penalty is paid, in words: "drive-through", "stop-go 10 s", "5 s added". */
	inline FString PenaltyKind(int32 Kind, int32 Seconds)
	{
		return Kind == ApexPenalty::DriveThrough ? FString(TEXT("drive-through"))
			: Kind == ApexPenalty::StopGo ? FString::Printf(TEXT("stop-go %d s"), Seconds)
			: FString::Printf(TEXT("%d s added"), Seconds);
	}

	/** The rest: "heavy · 9.5 m/s", "cut the course", "drive-through · jump start". */
	inline FString Detail(const FApexIncident& Incident)
	{
		switch (Incident.Kind)
		{
		case ApexIncident::Contact:
			return FString::Printf(TEXT("%s · %.1f m/s"),
				Incident.Severity == ApexIncident::Heavy ? TEXT("heavy") : Incident.Severity == ApexIncident::Light ? TEXT("light") : TEXT("a touch"),
				Incident.ClosingMps);
		case ApexIncident::YellowPass:
			return TEXT("in a yellow sector");
		case ApexIncident::TrackLimits:
			return Incident.bCut ? TEXT("cut the course") : TEXT("four wheels off");
		case ApexIncident::Penalty:
			return FString::Printf(TEXT("%s · %s"), *PenaltyKind(Incident.Penalty, Incident.Seconds), *Offence(Incident.Offence));
		case ApexIncident::PenaltyCleared:
			return FString::Printf(TEXT("%s · %s, taken back"),
				Incident.Penalty == ApexPenalty::DriveThrough ? TEXT("drive-through") : Incident.Penalty == ApexPenalty::StopGo ? TEXT("stop-go") : TEXT("time"),
				*Offence(Incident.Offence));
		default:
			return FString();
		}
	}

	/** A second of the race as the list shows it: "12:04.5". */
	inline FString RaceClock(double Seconds)
	{
		Seconds = FMath::Max(0.0, Seconds);
		const int32 Minutes = FMath::FloorToInt32(Seconds / 60.0);
		return FString::Printf(TEXT("%d:%04.1f"), Minutes, Seconds - Minutes * 60.0);
	}

	/**
	 * The second of a replay (its header's StartTick and TickRate, the
	 * recorder's TickOffset of cut garage time) a server tick falls on;
	 * false when the replay does not reach back to it.
	 */
	inline bool ReplaySecondsOf(int64 ServerTick, int64 StartTick, int32 TickRate, int64 TickOffset, double& OutSeconds)
	{
		const int64 Tick = ServerTick - TickOffset;
		if (TickRate <= 0 || StartTick < 0 || Tick < StartTick)
		{
			return false;
		}
		OutSeconds = static_cast<double>(Tick - StartTick) / TickRate;
		return true;
	}

	/** Where a jump into the replay puts it: LeadSeconds before the incident. */
	inline double JumpSeconds(double IncidentSeconds)
	{
		return FMath::Max(0.0, IncidentSeconds - LeadSeconds);
	}
}
