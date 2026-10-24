#pragma once

#include "CoreMinimal.h"
#include "ApexSpectatorStream.h"
#include "Tasks/Task.h"

/**
 * Writing the spectator stream (docs/game/spectator.md) on the client: the record
 * encoders, byte for byte as `server/protocol/src/spectator.rs` writes them (the
 * golden bytes pin both: `ApexSim.Spectator.Writer`), and an `.apxs` file
 * built from timed records a block at a time, which is how a replay of a race
 * the client took part in, or watched, is saved (UApexReplayRecorder).
 *
 * The zlib in a block need not match the server's byte for byte (two
 * compressors are free to differ); every reader inflates it the same.
 */
namespace ApexSpectator
{
	APEXSIMNET_API TArray<uint8> EncodeHeader(const FApexStreamHeader& Header);
	APEXSIMNET_API TArray<uint8> EncodeRoster(const FApexStreamRoster& Roster);
	/** One part holding every row: a file is not cut to a datagram's size. */
	APEXSIMNET_API TArray<uint8> EncodeFrame(const FApexStreamFrame& Frame);
	/** LapTiming, TrackSectors, SessionState, Finish, Retired, PitStop, Contact and PitService; empty for another kind. */
	APEXSIMNET_API TArray<uint8> EncodeEvent(const FApexStreamEvent& Event);
	APEXSIMNET_API TArray<uint8> EncodePath(const FApexStreamPath& Path);
	/** A road slice, as `spectator::StreamRoad::encode`. */
	APEXSIMNET_API TArray<uint8> EncodeRoad(const FApexStreamRoad& Road);
	/** A hotlap replay's ghost (record 10, the client's own). */
	APEXSIMNET_API TArray<uint8> EncodeGhost(const FApexStreamGhost& Ghost);
	/** The slow state, as `spectator::StreamState::encode`. */
	APEXSIMNET_API TArray<uint8> EncodeState(const FApexStreamState& State);
}

/**
 * An `.apxs` file in the making. Timed records (frames, events, roster
 * changes) are added in tick order and packed a block at a time off the game
 * thread, so a long race is held compressed; the preamble (header, roster,
 * path, the events a viewer needs first) is given at the end, when the header
 * knows how long the content ran.
 *
 * With the physics library a block is ten seconds of records laid out by
 * column (`apx_stream_block_pack`, the server's own writer's code) and the
 * file is version 2, as the server writes; without it, or when a block will
 * not pack, a block is plain zlib of a second's records and the file version
 * 1 (`ApexSim.Spectator.Writer`, `.WriterV2`).
 */
class APEXSIMNET_API FApexStreamWriter
{
public:
	/** Seconds a column block spans (`spectator::BLOCK_SECONDS`); a plain one spans one. */
	static constexpr int32 ColumnBlockSeconds = 10;

	/** The physics library is linked and speaks this build's ABI: blocks can be packed by column. */
	static bool CanPackColumns();

	/** InTickRate: the ticks of a second in the stream; bInColumns: version 2 column blocks. */
	explicit FApexStreamWriter(int32 InTickRate = 420, bool bInColumns = CanPackColumns());

	/** A timed record body; ticks never go back. */
	void Add(int64 Tick, TArrayView<const uint8> Body);

	bool IsEmpty() const { return Blocks.Num() == 0 && InFlight.Num() == 0 && PendingRecords == 0; }
	int32 NumRecords() const { return TotalRecords; }
	/** What the file holds so far, in bytes: packed blocks, and the raw size of those still packing. */
	int64 StoredBytes() const { return StoredBlockBytes + InFlightRawBytes + PendingRaw.Num(); }
	bool WritesColumns() const { return bColumns; }

	/**
	 * The whole file: magic, the preamble, every block (the open one too,
	 * packed for this copy; it waits for any still packing), the index and the
	 * trailer. Version 2 when any block is a column block. The writer can go
	 * on taking records afterwards.
	 */
	TArray<uint8> ToBytes(const FApexStreamHeader& Header, const FApexStreamRoster& Roster, const FApexStreamPath* Path,
		TConstArrayView<FApexStreamEvent> Preamble) const;

	void Reset();

private:
	struct FBlock
	{
		int64 FirstTick = 0;
		int64 LastTick = 0;
		int32 Records = 0;
		/** A column block (record 11) rather than a plain one (record 5). */
		bool bColumns = false;
		/** The block record's body, without its length. */
		TArray<uint8> Body;
	};

	/** The block of framed records Raw: by column when bColumns and the library packs it, else plain. */
	static FBlock Pack(int64 FirstTick, int64 LastTick, int32 Records, const TArray<uint8>& Raw, bool bColumns);
	/** The open block to a packing task. */
	void Flush();
	/** Blocks whose packing has finished, in order, into Blocks. */
	void Harvest();

	int32 BlockTicks = 420;
	bool bColumns = false;
	TArray<FBlock> Blocks;
	/** Blocks being packed, oldest first; mutable so ToBytes can wait for them. */
	mutable TArray<UE::Tasks::TTask<FBlock>> InFlight;
	TArray<int32> InFlightRaw;
	int64 InFlightRawBytes = 0;
	TArray<uint8> PendingRaw;
	int64 PendingFirstTick = 0;
	int64 PendingLastTick = 0;
	int32 PendingRecords = 0;
	int32 TotalRecords = 0;
	int64 StoredBlockBytes = 0;
};
