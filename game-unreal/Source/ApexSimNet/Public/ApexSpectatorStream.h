#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * The spectator stream (`.apxs`): what a viewer of a race receives, from a
 * file or from the server (docs/game/spectator.md; `server/src/spectator.rs` is
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
	constexpr int32 RowSize = 66;
	/** The third layout (wheels, no ground or race flags): read with both clear. */
	constexpr int32 RowSizeV3 = 64;
	/** The second layout (tyres, no wheels): read with the wheels unknown. */
	constexpr int32 RowSizeV2 = 52;
	/** A wheel's slip in the row is its multiple of the tyre's peak times this (`spectator::ROW_SLIP_SCALE`). */
	constexpr float RowSlipScale = 50.0f;
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
	constexpr uint8 RecordRoad = 8;
	constexpr uint8 RecordState = 9;
	/** The ghost of a hotlap replay (the record lap's trace): written by the client's recorder only. */
	constexpr uint8 RecordGhost = 10;

	/** Bytes of one car in a state record as written now; a newer writer only appends. */
	constexpr int32 ExtraSize = 49;
	/** ... as streams wrote them before the discs and the stint's energy (two dents a zone). */
	constexpr int32 ExtraSizeTwoDents = 44;
	/** ... as streams wrote it before the second dents (one dent a zone): still read ... */
	constexpr int32 ExtraSizeOneDent = 40;
	/** ... and as the first streams wrote it (no damage centres): still read. */
	constexpr int32 ExtraSizeMin = 36;
	/** `FApexStreamCarExtra::FuelDl` of a tank the stream does not know. */
	constexpr uint16 FuelUnknown = 0xFFFF;
	/** `FApexStreamCarExtra::BrakeWear` of a brake the stream does not know. */
	constexpr uint8 BrakeWearUnknown = 255;

	constexpr uint8 EventLapTiming = 1;
	constexpr uint8 EventTrackSectors = 2;
	constexpr uint8 EventSessionState = 3;
	constexpr uint8 EventFinish = 4;
	constexpr uint8 EventRetired = 5;
	constexpr uint8 EventPitStop = 6;
	constexpr uint8 EventContact = 7;
	constexpr uint8 EventPitService = 8;

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
	/**
	 * Each wheel's slip ratio and slip angle in ApexSpectator::RowSlipScale-ths
	 * of the tyre's peak, and the surface under it (0 road, 1 curb, 2 off),
	 * FL FR RL RR: what a replay sounds its tyres and kerbs by. bWheels is
	 * false, and all 0, from a row before them.
	 */
	int8 WheelSlipRatio[4] = {0, 0, 0, 0};
	int8 WheelSlipAngle[4] = {0, 0, 0, 0};
	uint8 WheelSurface[4] = {0, 0, 0, 0};
	bool bWheels = false;
	/** The soft ground under each tyre, two bits a wheel (`CompactCarState::ground`); 0 from an older stream. */
	uint8 Ground = 0;
	/** Race control's flags for the car (`CompactCarState::race_flags`); 0 from an older stream. */
	uint8 RaceFlags = 0;

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
	/** PitService: the crew's plan for the stop; its CarIndex is the stream's. */
	FApexPitService PitService;
};

/** The centerline every `SpacingM`, where a broadcast camera stands. */
struct APEXSIMNET_API FApexStreamPath
{
	uint32 Epoch = 0;
	float SpacingM = 10.0f;
	/** Server frame, metres. */
	TArray<FVector2D> Points;
};

/**
 * A slice of the road state (`spectator::StreamRoad`, record 8): the stream's
 * copy of the `RoadState` message, written round-robin over the lap so a
 * showcase, a replay or the backdrop shows the road as it was.
 */
struct APEXSIMNET_API FApexStreamRoad
{
	uint32 Epoch = 0;
	int64 Tick = 0;
	/** SessionId is left empty: a stream's road is its own. */
	FApexRoadState Road;
};

/**
 * What one car carries that moves slowly (`spectator::CarExtra`): 40 bytes (36
 * in a stream from before the damage centres), little-endian, one per car in a state record. Zero (255 for the brake wear,
 * 0xFFFF for the tank) is "the stream does not know".
 */
struct APEXSIMNET_API FApexStreamCarExtra
{
	int32 CarIndex = 0;
	uint8 TyreKpa[4] = {0, 0, 0, 0};
	/** Percent of the worst flat spot. */
	uint8 FlatSpot[4] = {0, 0, 0, 0};
	uint16 BrakeC[4] = {0, 0, 0, 0};
	uint16 FuelDl = ApexSpectator::FuelUnknown;
	uint8 WaterC = 0;
	/** Tread shoulders, inner then outer per tyre. */
	uint8 TyreEdgesC[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	uint8 BrakeWear[4] = {255, 255, 255, 255};
	/** Hybrid charge, lap budget, stint budget: 255 for none. */
	uint8 ErsPct = 255;
	uint8 ErsLapPct = 255;
	uint8 ErsStintPct = 255;
	uint8 TowPct = 0;
	/** Where each zone was hit, 127ths: front, rear (across, + left), left, right (along, + forward). */
	int8 DamageAt[4] = {0, 0, 0, 0};
	/** The entry had the damage centres (a 40-byte one or longer). */
	bool bHasDamageAt = false;
	/** Each zone's second dent, as DamageAt; -128 for a zone with one (and from an older entry). */
	int8 DamageAt2[4] = {-128, -128, -128, -128};
	/** Each disc's wear, percent, 255 a disc that never wears (and from an older stream). */
	uint8 DiscWear[4] = {255, 255, 255, 255};
	/** The stint's energy left, percent, 255 without the rule (and from an older stream). */
	uint8 StintEnergyPct = 255;

	static bool Read(const uint8* Bytes, int32 Len, FApexStreamCarExtra& Out);
	/** The entry written back as its ApexSpectator::ExtraSize bytes, for tests. */
	void Write(TArray<uint8>& Out) const;
	/** Fill what a row leaves unknown: pressures, flat spots, brakes, tank, coolant, edges, hybrid, tow. */
	void ApplyTo(FApexCarTelemetry& Car) const;
	/** A live car's extras, the way the server's encoder takes them: how a client-side replay writes its state records. */
	static FApexStreamCarExtra FromTelemetry(const FApexCarTelemetry& Car);
};

/**
 * The slow state of a stream (`spectator::StreamState`, record 9): the sky as
 * it is now, a timed race's clock and the extras of every car, a few times a
 * second.
 */
struct APEXSIMNET_API FApexStreamState
{
	uint32 Epoch = 0;
	int64 Tick = 0;
	/** `Sky.bValid` is false when the record carried none. */
	FApexSkyNow Sky;
	/** -1 when the record carried no clock. */
	int32 RaceLeftMs = -1;
	int32 RaceFinalLap = 0;
	TArray<FApexStreamCarExtra> Cars;
	/** The race's yellow sectors (`yellow_flags`, appended), as FApexTelemetryFrame carries them; none from an older stream. */
	int32 YellowSectorM = 0;
	TArray<TPair<int32, int32>> YellowSectors;
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

/** A hotlap replay's ghost: the record lap's trace, as `GhostLap` carried it (record 10). */
struct APEXSIMNET_API FApexStreamGhost
{
	uint32 Epoch = 0;
	int64 Tick = 0;
	FApexGhostLap Lap;
};

/** Any one record, decoded. */
struct APEXSIMNET_API FApexStreamRecord
{
	uint8 Type = 0;
	FApexStreamGhost Ghost;
	FApexStreamHeader Header;
	FApexStreamRoster Roster;
	FApexStreamFrame Frame;
	FApexStreamEvent Event;
	FApexStreamPath Path;
	FApexStreamRoad Road;
	FApexStreamState State;
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
	/** Road slices of the current epoch since the last call, in order. */
	TArray<FApexStreamRoad> TakeRoads();
	/** The ghosts (hotlap replays) applied since the last call. */
	TArray<FApexStreamGhost> TakeGhosts();

	/** The latest slow state of the current epoch (the sky, the race clock, each car's extras); null before the first. */
	const FApexStreamState* GetState() const { return bHasState ? &State : nullptr; }

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
	bool bHasState = false;
	FApexStreamState State;
	FApexStreamHeader Header;
	FApexStreamRoster Roster;
	FApexStreamPath Path;

	/** A split frame being put together. */
	bool bPending = false;
	FApexStreamFrame Pending;
	uint32 PendingMask = 0;

	TArray<FApexTelemetryFrame> Frames;
	TArray<FApexStreamEvent> Events;
	TArray<FApexStreamRoad> Roads;
	TArray<FApexStreamGhost> Ghosts;
	int32 DroppedFrames = 0;
	int32 IncompleteFrames = 0;
	int32 AppliedFrames = 0;
};
