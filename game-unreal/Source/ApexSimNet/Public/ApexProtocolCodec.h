#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

class FMsgPackWriter;

/**
 * Encodes ClientMessages to, and decodes ServerMessages from, MessagePack.
 *
 * Payloads only — the 4-byte big-endian length prefix is applied by
 * FApexTcpConnection when the frame goes on the wire.
 *
 * See the case-convention notes at the top of ApexProtocolTypes.h before
 * touching any key literal in here.
 */
namespace ApexProtocol
{
	/** Whether TrackConfigSummary::Centerline is parsed rather than skipped. */
	APEXSIMNET_API bool ShouldParseCenterline();

	// --- Client -> server -----------------------------------------------------
	// Each returns a complete MessagePack payload for one message.

	APEXSIMNET_API TArray<uint8> EncodeAuthenticate(const FString& Token, const FString& PlayerName);
	APEXSIMNET_API TArray<uint8> EncodeHeartbeat(uint32 ClientTick);
	APEXSIMNET_API TArray<uint8> EncodeSelectCar(const FString& CarConfigId);
	APEXSIMNET_API TArray<uint8> EncodeRequestLobbyState();
	APEXSIMNET_API TArray<uint8> EncodeCreateSession(
		const FString& TrackConfigId,
		uint8 MaxPlayers,
		uint8 AiCount,
		uint8 LapLimit,
		EApexSessionKind SessionKind);
	APEXSIMNET_API TArray<uint8> EncodeJoinSession(const FString& SessionId);
	APEXSIMNET_API TArray<uint8> EncodeJoinAsSpectator(const FString& SessionId);
	APEXSIMNET_API TArray<uint8> EncodeLeaveSession();
	APEXSIMNET_API TArray<uint8> EncodeStartSession();
	APEXSIMNET_API TArray<uint8> EncodeDisconnect();
	APEXSIMNET_API TArray<uint8> EncodeSetGameMode(EApexGameMode Mode);
	APEXSIMNET_API TArray<uint8> EncodeStartCountdown(uint16 CountdownSeconds, EApexGameMode NextMode);

	// --- Server -> client -----------------------------------------------------

	/**
	 * Decodes one framed payload. Returns false and fills OutError only on a
	 * malformed frame; an unrecognised variant decodes successfully as
	 * EApexServerMessageType::Unknown so a newer server cannot break us.
	 */
	APEXSIMNET_API bool DecodeServerMessage(
		TArrayView<const uint8> Payload,
		FApexServerMessage& OutMessage,
		FString& OutError);
}
