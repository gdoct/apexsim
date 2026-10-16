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

	/** `ResumeToken`: an earlier AuthSuccess's, to be that player again; empty for a new one. */
	APEXSIMNET_API TArray<uint8> EncodeAuthenticate(const FString& Token, const FString& PlayerName, const FString& ResumeToken = FString());
	/** `PingMs`: the last measured round trip, sent as `ping_ms` for the operator dashboard; negative (not yet measured) leaves it off. */
	APEXSIMNET_API TArray<uint8> EncodeHeartbeat(uint32 ClientTick, int32 PingMs = -1);
	/** `Livery`: 0 the car as authored, 1.. the car's `[[livery]]` tables. */
	APEXSIMNET_API TArray<uint8> EncodeSelectCar(const FString& CarConfigId, int32 Livery = 0);
	APEXSIMNET_API TArray<uint8> EncodeRequestLobbyState();
	/**
	 * A watched hotlap's own choices (CreateSession's `watch`, written only
	 * when given): watched as a stream (SpectatorJoined and the records, not
	 * a SessionJoined) and the setup its car runs.
	 */
	struct FHotlapWatchOptions
	{
		bool bStream = false;
		bool bHasSetup = false;
		FApexCarSetup Setup;
	};

	APEXSIMNET_API TArray<uint8> EncodeCreateSession(
		const FString& TrackConfigId,
		uint8 MaxPlayers,
		uint8 AiCount,
		uint8 LapLimit,
		EApexSessionKind SessionKind,
		const FApexAllowedAssists& AllowedAssists,
		const FApexSessionConditions& Conditions,
		EApexDamageLevel Damage = EApexDamageLevel::Full,
		int32 AiSkill = ApexAiSkill::Mixed,
		int32 RaceSeconds = 0,
		const TArray<FString>& GridOrder = TArray<FString>(),
		int32 QualifyingSeconds = 0,
		const FApexWeekendFormat& Weekend = FApexWeekendFormat(),
		const FHotlapWatchOptions* Watch = nullptr,
		int32 QualifyingCutoffPct = ApexQualifyingCutoff::None,
		int32 MandatoryStops = ApexMandatoryStops::None,
		bool bCompoundChange = false);
	/** The host moves a race weekend on to its next session (no fields). */
	APEXSIMNET_API TArray<uint8> EncodeAdvanceWeekend();
	/** `GiveUpSeat`: turn down the rejoin offer; the held seat's car leaves its session. */
	APEXSIMNET_API TArray<uint8> EncodeGiveUpSeat();
	/** Asks for a track's fastest laps (a car id, or empty for every car); answered with Leaderboard. */
	APEXSIMNET_API TArray<uint8> EncodeRequestLeaderboard(const FString& TrackConfigId, const FString& CarConfigId);
	/** Asks for the qualifying results stored for a track; answered with QualifyingResults. */
	APEXSIMNET_API TArray<uint8> EncodeRequestQualifyingResults(const FString& TrackConfigId);
	APEXSIMNET_API TArray<uint8> EncodeJoinSession(const FString& SessionId);
	APEXSIMNET_API TArray<uint8> EncodeJoinAsSpectator(const FString& SessionId);
	APEXSIMNET_API TArray<uint8> EncodeLeaveSession();
	APEXSIMNET_API TArray<uint8> EncodeStartSession();
	APEXSIMNET_API TArray<uint8> EncodeDisconnect();
	APEXSIMNET_API TArray<uint8> EncodeSetGameMode(EApexGameMode Mode);
	APEXSIMNET_API TArray<uint8> EncodeStartCountdown(uint16 CountdownSeconds, EApexGameMode NextMode);
	/** The host puts the race's grid up; it counts CountdownSeconds into the race once every driver is ready. */
	APEXSIMNET_API TArray<uint8> EncodeOpenGrid(uint16 CountdownSeconds);
	/** A driver on the grid is ready to start, or not any more. */
	APEXSIMNET_API TArray<uint8> EncodeSetReady(bool bReady);
	/** The livery this driver's car wears from now on, in a session: 0 as authored, 1.. its liveries. */
	APEXSIMNET_API TArray<uint8> EncodeSetLivery(uint8 Livery);
	/**
	 * ABS and traction control are the driver's own; the session's allowed
	 * set is applied by the server. Damage is no aid: it is the session's
	 * (EncodeCreateSession).
	 */
	APEXSIMNET_API TArray<uint8> EncodeSetDriverAids(
		bool bAutoGearbox, bool bSteeringAssist, bool bAbs, EApexTractionControl TractionControl,
		bool bPitAutopilot = true);
	/** Every knob is sent, clamped here as the server will clamp it again. */
	APEXSIMNET_API TArray<uint8> EncodeSetCarSetup(const FApexCarSetup& Setup);
	/** The driver's orders for their next stop; fields at their default are left out. */
	APEXSIMNET_API TArray<uint8> EncodeSetPitPlan(const FApexPitPlan& Plan);
	/** A hotlap driver asks to be put in the garage or out on the run-up. */
	/** bColdTyres asks to go out as from the garage (blankets or the air) rather
	 *  than at the compound's optimum; it is only written when set, so the bytes
	 *  an older server reads are unchanged. */
	APEXSIMNET_API TArray<uint8> EncodeHotlapRelocate(EApexHotlapDestination Destination, bool bColdTyres = false);
	/** A stuck driver asks for their car back: onto the track where it is, or to its box. */
	APEXSIMNET_API TArray<uint8> EncodeRecoverCar(EApexRecoverDestination Destination);
	/** Asks for the trace of the driver's record lap here, answered with GhostLap. */
	APEXSIMNET_API TArray<uint8> EncodeRequestGhost();
	/** Asks which showcases the server plays; answered with Showcases. */
	APEXSIMNET_API TArray<uint8> EncodeListShowcases();
	/** Watch a showcase channel (empty: the server's first); answered with SpectatorJoined and the stream. */
	APEXSIMNET_API TArray<uint8> EncodeSpectateShowcase(const FString& Id);
	APEXSIMNET_API TArray<uint8> EncodeLeaveSpectate();
	/** Watch a live session as a spectator stream; answered with SpectatorJoined (Live) and the stream. */
	APEXSIMNET_API TArray<uint8> EncodeSpectateSession(const FString& SessionId);

	// --- Client -> server over UDP -------------------------------------------
	// Sent as datagrams: no length prefix, unlike the TCP stream. The server
	// decodes with `rmp_serde::from_slice`, which accepts either encoding, so
	// these keep using the named encoder. Every datagram goes out wrapped by
	// SealUdpDatagram; the server drops a bare one.

	APEXSIMNET_API TArray<uint8> EncodeUdpHandshake(const FString& UdpToken);
	APEXSIMNET_API TArray<uint8> EncodePlayerInput(uint32 ServerTickAck, const FApexPlayerInput& Input);
	/**
	 * Seals one outbound datagram (`server/src/udp_seal.rs`): a 0xC1 marker,
	 * the big-endian sequence number, 16 bytes of HMAC-SHA1 over the sequence
	 * number and the payload under the UTF-8 bytes of `AuthSuccess.udp_key`,
	 * then the payload. The server accepts a connection's datagrams only in
	 * rising sequence order, so Seq must go up with every send, the handshake
	 * retries included.
	 */
	APEXSIMNET_API TArray<uint8> SealUdpDatagram(const FString& UdpKey, uint64 Seq, TArrayView<const uint8> Payload);

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
	 * UDP carries three different encodings: `UdpHandshakeAck` arrives named (a
	 * `{"type": ...}` map, same as TCP), `TelemetryCompact` and
	 * `DriverFeedback` arrive positional (a `["TelemetryCompact", [...]]`
	 * array), and a spectator stream frame is a bare record body (an array
	 * opening on a small integer, ApexSpectatorStream.h), which comes back as
	 * `SpectatorRecord` with the body framed in `SpectatorRecords`. This
	 * dispatches on the leading bytes and hands off to the right decoder.
	 */
	APEXSIMNET_API bool DecodeUdpMessage(
		TArrayView<const uint8> Payload,
		FApexServerMessage& OutMessage,
		FString& OutError);
}
