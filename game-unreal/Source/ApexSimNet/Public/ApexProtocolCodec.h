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

	// --- Client -> server over UDP -------------------------------------------
	// Sent as bare datagrams: no length prefix, unlike the TCP stream. The
	// server decodes with `rmp_serde::from_slice`, which accepts either
	// encoding, so these keep using the named encoder.

	APEXSIMNET_API TArray<uint8> EncodeUdpHandshake(const FString& UdpToken);
	APEXSIMNET_API TArray<uint8> EncodePlayerInput(uint32 ServerTickAck, const FApexPlayerInput& Input);

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

	/**
	 * Decodes one UDP datagram.
	 *
	 * UDP carries two different encodings: `UdpHandshakeAck` arrives named (a
	 * `{"type": ...}` map, same as TCP) while `TelemetryCompact` arrives
	 * positional (a `["TelemetryCompact", [...]]` array). This dispatches on the
	 * leading format byte and hands off to the right decoder.
	 */
	APEXSIMNET_API bool DecodeUdpMessage(
		TArrayView<const uint8> Payload,
		FApexServerMessage& OutMessage,
		FString& OutError);
}
