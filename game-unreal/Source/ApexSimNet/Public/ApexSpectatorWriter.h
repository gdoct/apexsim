#pragma once

#include "CoreMinimal.h"
#include "ApexSpectatorStream.h"

/**
 * Writing the spectator stream (docs/SPECTATOR.md) on the client: the record
 * encoders, byte for byte as `server/src/spectator.rs` writes them (the
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
	/** LapTiming, TrackSectors, SessionState, Finish, Retired, PitStop and Contact; empty for another kind. */
	APEXSIMNET_API TArray<uint8> EncodeEvent(const FApexStreamEvent& Event);
	APEXSIMNET_API TArray<uint8> EncodePath(const FApexStreamPath& Path);
}

/**
 * An `.apxs` file in the making. Timed records (frames, events, roster
 * changes) are added in tick order and compressed a block (a second of ticks)
 * at a time, so a long race is held compressed; the preamble (header, roster,
 * path, the events a viewer needs first) is given at the end, when the header
 * knows how long the content ran.
 */
class APEXSIMNET_API FApexStreamWriter
{
public:
	/** Ticks a block spans (a second at the stream's tick rate). */
	explicit FApexStreamWriter(int32 InBlockTicks = 420) : BlockTicks(FMath::Max(1, InBlockTicks)) {}

	/** A timed record body; ticks never go back. */
	void Add(int64 Tick, TArrayView<const uint8> Body);

	bool IsEmpty() const { return Blocks.Num() == 0 && PendingRecords == 0; }
	int32 NumRecords() const { return TotalRecords; }
	/** What the file holds so far, compressed, in bytes. */
	int64 StoredBytes() const { return StoredBlockBytes + PendingRaw.Num(); }

	/**
	 * The whole file: magic, the preamble, every block (the open one too,
	 * compressed for this copy), the index and the trailer. The writer can
	 * go on taking records afterwards.
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
		int32 RawLen = 0;
		TArray<uint8> Zlib;
	};

	static FBlock Compress(int64 FirstTick, int64 LastTick, int32 Records, const TArray<uint8>& Raw);
	void Flush();

	int32 BlockTicks = 420;
	TArray<FBlock> Blocks;
	TArray<uint8> PendingRaw;
	int64 PendingFirstTick = 0;
	int64 PendingLastTick = 0;
	int32 PendingRecords = 0;
	int32 TotalRecords = 0;
	int64 StoredBlockBytes = 0;
};
