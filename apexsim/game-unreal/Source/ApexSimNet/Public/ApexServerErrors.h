#pragma once

#include "CoreMinimal.h"

/**
 * What the player reads for a server's `Error { code, message }`
 * (docs/game/client.md, "Session lobby"). The server's messages are short
 * phrases for logs and the dashboard; the refusals a player meets in a race
 * event (docs/server/sessions.md, "Events across restarts") are put as a
 * sentence that says what to do, and any other refusal (403) reads as the
 * server's own sentence. The rest keep the code beside the message.
 */
namespace ApexServerError
{
	/** The player's sentence for a server error. */
	inline FString Describe(int32 Code, const FString& Message)
	{
		if (Code == 403)
		{
			// events::EventStore::admit and the SetLivery refusal (dispatch.rs).
			if (Message == TEXT("This event takes drivers by their proved driver id"))
			{
				return TEXT("This race event takes drivers by their driver key, and this connection has not proved one. Reconnect to the server and join again.");
			}
			if (Message == TEXT("Not entered in this event"))
			{
				return TEXT("You are not entered in this race event, and its entry list is closed.");
			}
			if (Message == TEXT("This event's entry list is full"))
			{
				return TEXT("This race event's entry list is full: every seat has a driver entered.");
			}
			if (Message == TEXT("The livery is fixed for this event"))
			{
				return TEXT("Your livery is the one you entered this race event with, for every session of it.");
			}
			FString Sentence = Message.TrimStartAndEnd();
			if (Sentence.IsEmpty())
			{
				return TEXT("The server refused that.");
			}
			if (!Sentence.EndsWith(TEXT(".")) && !Sentence.EndsWith(TEXT("!")) && !Sentence.EndsWith(TEXT("?")))
			{
				Sentence += TEXT(".");
			}
			return Sentence;
		}
		return FString::Printf(TEXT("Server error %d: %s"), Code, *Message);
	}
}
