#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * The spectator stream (`.apxs`): what a viewer of a race receives, from a
 * file or from the server (docs/SPECTATOR.md; `server/src/spectator.rs` is
 * the format's definition and prints the golden bytes these are tested on).
 *
 * A stream is a sequence of self-contained records, each a positional
 * MessagePack array whose first element is the record type and, for every
 * record a viewer receives, whose second is the epoch as a full `uint 32`:
 *
 *   [u32 big-endian length][MessagePack body]       on disk and inside a TCP message
 *   [MessagePack body]                              one frame per UDP datagram
 *
 * Pure data and maths: no world, no net. FApexSpectatorPlayer turns records
 * into the roster and telemetry frames the race director already draws.
 */
namespace ApexSpectator
{
	constexpr uint8 FormatVersion = 1;
	constexpr uint16 FileVersion = 1;
	/** Bytes of one car in a frame as written now; a newer writer only appends. */
	constexpr int32 RowSize = 52;
	/** The first layout's row (no tyres): the shortest a reader takes. */
	constexpr int32 RowSizeV1 = 44;
	/** `FApexStreamCarRow::TyreWear` of a tyre the stream does not know. */
	constexpr uint8 TyreWearUnknown = 255;
	constexpr int32 NoCountdown = 0xFFFF;

	constexpr uint8 RecordHeader = 1;
	constexpr uint8 RecordRoster = 2;
	constexpr uint8 RecordFrame = 3;
	constexpr uint8 RecordEvent = 4;
	constexpr uint8 RecordBlock = 5;
	constexpr uint8 RecordIndex = 6;
	constexpr uint8 RecordPath = 7;

	constexpr uint8 EventLapTiming = 1;
	constexpr uint8 EventTrackSectors = 2;
	constexpr uint8 EventSessionState = 3;
	constexpr uint8 EventFinish = 4;
	constexpr uint8 EventRetired = 5;
	constexpr uint8 EventPitStop = 6;
	constexpr uint8 EventContact = 7;

	/** `FApexStreamCarRow::Status` bits. */
	constexpr uint8 StatusOnTrack = 1;
	constexpr uint8 StatusColliding = 2;
	constexpr uint8 StatusInGarage = 4;
	constexpr uint8 StatusRetired = 8;
	constexpr uint8 StatusFinished = 16;
}

struct APEXSIMNET_API FApexStreamTrack
{
	FString TrackId;
	/** The YAML's stem: what the export and the catalog are named by. */
	FString Stem;
	FString DisplayName;
	/** `content_crc` of the track YAML the race was simulated on; 0 unknown. */
	uint32 SourceCrc = 0;
	float LengthM = 0.0f;
};

struct APEXSIMNET_API FApexStreamHeader
{
	uint32 Epoch = 0;
	uint8 Version = 0;
	FString StreamId;
	int32 TickRate = 420;
	int32 FrameRate = 30;
	int32 RowSize = ApexSpectator::RowSize;
	FApexStreamTrack Track;
	FApexSessionConditions Conditions;
	EApexSessionKind SessionKind = EApexSessionKind::Multiplayer;
	EApexGameMode GameMode = EApexGameMode::Race;
	int32 LapLimit = 0;
	/** The tick the lights went out; -1 when the content holds no start. */
	int64 RaceStartTick = -1;
	int64 StartTick = 0;
	int64 EndTick = 0;
	/** How the file was rendered; -1 / NaN when not recorded. */
	int64 Seed = -1;
	float Score = 0.0f;
	bool bHasScore = false;

	double DurationSeconds() const { return TickRate > 0 ? static_cast<double>(EndTick - StartTick) / TickRate : 0.0; }
};

struct APEXSIMNET_API FApexStreamRosterEntry
{
	int32 CarIndex = 0;
	FString CarConfigId;
	/** `content_crc` of the car.toml the car was simulated from; 0 unknown. */
	uint32 ContentCrc = 0;
	int32 Livery = 0;
	FString Name;
	bool bIsAi = true;
};

struct APEXSIMNET_API FApexStreamRoster
{
	uint32 Epoch = 0;
	int32 Revision = 0;
	TArray<FApexStreamRosterEntry> Entries;

	/** The roster the race director spawns a field from. */
	FApexSessionRoster ToSessionRoster(const FString& SessionId) const;
};

/**
 * One car in a frame (`spectator::CarRow`): 52 bytes, little-endian. The
 * first 44 are format version 1's; a stream whose header says 44 reads with
 * its tyres unknown.
 */
struct APEXSIMNET_API FApexStreamCarRow
{
	int32 CarIndex = 0;
	uint8 Status = 0;
	int32 XMm = 0;
	int32 YMm = 0;
	int32 ZMm = 0;
	uint16 Yaw = 0;
	int16 Pitch = 0;
	int16 Roll = 0;
	uint16 SpeedCms = 0;
	int8 Steering = 0;
	uint8 Throttle = 0;
	uint8 Brake = 0;
	int8 Gear = 0;
	uint16 EngineRpm = 0;
	uint16 Lap = 0;
	uint32 StationCm = 0;
	uint8 FinishPosition = 0;
	uint8 LapFlags = 0;
	uint8 PitFlags = 0;
	uint8 Compound = 255;
	uint8 Damage[5] = {0, 0, 0, 0, 0};
	uint8 ErsFlags = 0;
	/** Wear, percent, FL FR RL RR; ApexSpectator::TyreWearUnknown when unknown. */
	uint8 TyreWear[4] = {255, 255, 255, 255};
	/** Tread temperature, °C as `CompactCarState.tyre_c`; 0 when unknown. */
	uint8 TyreC[4] = {0, 0, 0, 0};

	/** Read a row of at least ApexSpectator::RowSizeV1 bytes; what a newer writer appended is ignored. */
	static bool Read(const uint8* Bytes, int32 Len, FApexStreamCarRow& Out);
	/** The row written back as its ApexSpectator::RowSize bytes, for tests and the clip writer. */
	void Write(TArray<uint8>& Out) const;

	/** As the race director reads a live car: what the row lacks (tyres, fuel...) is unknown. */
	FApexCarTelemetry ToTelemetry() const;

	/**
	 * A live car as a row, the way the server's encoder makes one: how a
	 * replay of a race the client received is written (UApexReplayRecorder).
	 * `FromTelemetry(Row.ToTelemetry())` is the row again.
	 */
	static FApexStreamCarRow FromTelemetry(const FApexCarTelemetry& Car);
};

struct APEXSIMNET_API FApexStreamFrame
{
	uint32 Epoch = 0;
	int64 Tick = 0;
	int32 RosterRevision = 0;
	EApexSessionState State = EApexSessionState::Lobby;
	/** -1 when nothing counts down. */
	int32 CountdownMs = -1;
	int32 Part = 0;
	int32 Parts = 1;
	TArray<uint8> Rows;

	/** The rows, `RowSize` bytes apart. */
	TArray<FApexStreamCarRow> Cars(int32 RowSize) const;
};

struct APEXSIMNET_API FApexStreamEvent
{
	uint32 Epoch = 0;
	int64 Tick = 0;
	/** One of ApexSpectator::Event*, or something newer. */
	uint8 Kind = 0;
	FApexLapTiming LapTiming;
	FApexTrackSectors Sectors;
	EApexSessionState State = EApexSessionState::Lobby;
	/** Finish, Retired, PitStop and Contact: the car. */
	int32 CarIndex = 0;
	/** The position, the reason, the phase (0 entered, 1 serviced, 2 left). */
	int32 Value = 0;
	/** Contact: the other car (255 unknown) and the car's speed, cm/s. */
	int32 Other = 255;
	int32 SpeedCms = 0;
};

/** The centerline every `SpacingM`, where a broadcast camera stands. */
struct APEXSIMNET_API FApexStreamPath
{
	uint32 Epoch = 0;
	float SpacingM = 10.0f;
	/** Server frame, metres. */
	TArray<FVector2D> Points;
};

struct APEXSIMNET_API FApexStreamBlock
{
	int64 FirstTick = 0;
	int64 LastTick = 0;
	int32 Records = 0;
	int32 RawLen = 0;
	TArray<uint8> Zlib;
};

struct APEXSIMNET_API FApexStreamIndexEntry
{
	int64 FirstTick = 0;
	int64 Offset = 0;
	int32 Records = 0;
};

/** Any one record, decoded. */
struct APEXSIMNET_API FApexStreamRecord
{
	uint8 Type = 0;
	FApexStreamHeader Header;
	FApexStreamRoster Roster;
	FApexStreamFrame Frame;
	FApexStreamEvent Event;
	FApexStreamPath Path;
	FApexStreamBlock Block;
	TArray<FApexStreamIndexEntry> Index;
};

namespace ApexSpectator
{
	/** The record type of a body without decoding it; 0 when the bytes are not a record. */
	APEXSIMNET_API uint8 RecordType(TArrayView<const uint8> Body);
	/** The epoch of a viewer-facing record without decoding it; false when the bytes carry none. */
	APEXSIMNET_API bool RecordEpoch(TArrayView<const uint8> Body, uint32& OutEpoch);
	/** The tick of a frame or event without decoding it. */
	APEXSIMNET_API bool RecordTick(TArrayView<const uint8> Body, int64& OutTick);
	/** Whether a UDP datagram is a stream record (a fixarray opening on a small integer). */
	APEXSIMNET_API bool IsRecordDatagram(TArrayView<const uint8> Datagram);

	/** Decode one record body. False with a reason on malformed bytes; an unknown type decodes with just `Type`. */
	APEXSIMNET_API bool DecodeRecord(TArrayView<const uint8> Body, FApexStreamRecord& Out, FString& OutError);

	/** Split a run of `[u32 length][body]` records into views of the bodies. */
	APEXSIMNET_API bool SplitFramed(TArrayView<const uint8> Run, TArray<TArrayView<const uint8>>& OutBodies, FString& OutError);
	/** `Body` with its length in front: the stream's framing. */
	APEXSIMNET_API void AppendFramed(TArray<uint8>& Out, TArrayView<const uint8> Body);
}

/**
 * An `.apxs` file: the plain preamble decoded, the blocks still compressed
 * until asked for. `Zandvoort.gt3.day.apxs` of twenty cars for two laps is
 * a few megabytes.
 */
class APEXSIMNET_API FApexStreamFile
{
public:
	static bool IsStreamFile(const FString& FilePath);

	bool LoadFromFile(const FString& FilePath, FString& OutError);
	bool LoadFromBytes(TArray<uint8>&& Bytes, FString& OutError);

	/**
	 * The header and roster alone, from the file's first kilobytes: what a
	 * catalog of showcase files needs, without reading megabytes of blocks.
	 */
	static bool ReadPreamble(const FString& FilePath, FApexStreamHeader& OutHeader, FApexStreamRoster& OutRoster, FString& OutError);

	const FApexStreamHeader& GetHeader() const { return Header; }
	const FApexStreamRoster& GetRoster() const { return Roster; }
	bool HasPath() const { return bHasPath; }
	const FApexStreamPath& GetPath() const { return Path; }
	/** The events ahead of the first block (`TrackSectors`). */
	const TArray<FApexStreamEvent>& GetPreamble() const { return Preamble; }
	const TArray<FApexStreamIndexEntry>& GetIndex() const { return Index; }
	/** The plain records ahead of the first block, framed: what a player applies before the content. */
	TArrayView<const uint8> GetPreambleBytes() const { return TArrayView<const uint8>(Bytes.GetData() + 6, static_cast<int32>(PreambleEnd - 6)); }
	int32 NumBlocks() const { return Index.Num(); }
	int64 NumBytes() const { return Bytes.Num(); }

	/** The record bodies of one block, inflated. */
	bool ReadBlock(int32 BlockIndex, TArray<TArray<uint8>>& OutBodies, FString& OutError) const;
	/** The block holding `Tick`: the last whose first tick is not after it. */
	int32 BlockForTick(int64 Tick) const;
	/** Every timed record body of the file, in order. */
	bool ReadAll(TArray<TArray<uint8>>& OutBodies, FString& OutError) const;

private:
	bool ReadRecordAt(int64 Offset, TArrayView<const uint8>& OutBody, int64& OutNext, FString& OutError) const;

	TArray<uint8> Bytes;
	FApexStreamHeader Header;
	FApexStreamRoster Roster;
	FApexStreamPath Path;
	bool bHasPath = false;
	TArray<FApexStreamEvent> Preamble;
	TArray<FApexStreamIndexEntry> Index;
	/** Byte offset of the first block (or the index), where the preamble ends. */
	int64 PreambleEnd = 6;
};

/**
 * Turns a stream's records into what the race director draws. Feed it every
 * record as it arrives (`Apply`), from either source, then drain: a header
 * starts an epoch (and resets everything), a roster with the current epoch
 * replaces the field, a frame whose epoch or roster revision is not the one
 * held is dropped (a stale datagram of another stream or loop), the parts of
 * a split frame are put together, events pass through with their tick.
 */
class APEXSIMNET_API FApexSpectatorPlayer
{
public:
	void Reset();

	/** Apply one record body. False with a reason when it does not decode. */
	bool Apply(TArrayView<const uint8> Body, FString& OutError);
	/** Apply a framed run of records (a TCP message, a file block). */
	bool ApplyFramed(TArrayView<const uint8> Run, FString& OutError);

	bool HasHeader() const { return bHasHeader; }
	const FApexStreamHeader& GetHeader() const { return Header; }
	bool HasRoster() const { return bHasRoster; }
	const FApexStreamRoster& GetRoster() const { return Roster; }
	bool HasPath() const { return bHasPath; }
	const FApexStreamPath& GetPath() const { return Path; }
	uint32 GetEpoch() const { return Header.Epoch; }

	/** Set by Apply when a header or roster arrived; cleared by the Take* calls. */
	bool TakeHeaderChanged() { const bool b = bHeaderChanged; bHeaderChanged = false; return b; }
	bool TakeRosterChanged() { const bool b = bRosterChanged; bRosterChanged = false; return b; }
	/** Frames completed since the last call, in tick order, as the director reads a live frame. */
	TArray<FApexTelemetryFrame> TakeFrames();
	TArray<FApexStreamEvent> TakeEvents();

	/** Frames thrown away for a wrong epoch or revision. */
	int32 GetDroppedFrames() const { return DroppedFrames; }
	/** Frames completed with a part missing. */
	int32 GetIncompleteFrames() const { return IncompleteFrames; }
	int32 GetAppliedFrames() const { return AppliedFrames; }

private:
	void Flush();
	void AcceptFrame(FApexStreamFrame&& Frame);

	bool bHasHeader = false;
	bool bHasRoster = false;
	bool bHasPath = false;
	bool bHeaderChanged = false;
	bool bRosterChanged = false;
	FApexStreamHeader Header;
	FApexStreamRoster Roster;
	FApexStreamPath Path;

	/** A split frame being put together. */
	bool bPending = false;
	FApexStreamFrame Pending;
	uint32 PendingMask = 0;

	TArray<FApexTelemetryFrame> Frames;
	TArray<FApexStreamEvent> Events;
	int32 DroppedFrames = 0;
	int32 IncompleteFrames = 0;
	int32 AppliedFrames = 0;
};
