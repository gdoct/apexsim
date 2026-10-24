#pragma once

#include "CoreMinimal.h"

class UApexNetSubsystem;
class UVerticalBox;
class UWidgetTree;

/**
 * The championship standings beside a race event's session: in the session
 * lobby and on the results screen (docs/game/client.md, "Session lobby").
 * The joined session names its event (`SessionJoined.Event`); the
 * championships come from `RequestChampionships`, asked for on that join,
 * and the server sends them again once the event's race is kept, so the
 * results count the race just run.
 */
namespace ApexChampionshipPanel
{
	/** Standings a championship shows before the player's own line. */
	constexpr int32 TopRows = 5;

	/**
	 * Box rebuilt with every championship the session's event is a round of:
	 * its name, the round and what the table counts, the fastest-lap rule and
	 * this round's fastest lap, then the first Top standings and the player's
	 * own line. False (and Box empty) when the session is not a championship
	 * round or the championships have not arrived.
	 */
	bool Fill(UWidgetTree& Tree, UVerticalBox& Box, const UApexNetSubsystem& Net, int32 Top = TopRows);
}
