#include "ApexSpectatorWriter.h"

#include "Misc/Compression.h"
#include "MsgPack/MsgPackWriter.h"

namespace
{
	/** `[type, epoch]`: how every record a viewer receives begins (`spectator::begin`). */
	void BeginRecord(FMsgPackWriter& Writer, int32 Fields, uint8 Type, uint32 Epoch)
	{
		Writer.WriteArrayHeader(Fields);
		Writer.WriteUInt(Type);
		Writer.WriteUInt32Fixed(Epoch);
	}

	/** An optional figure the client keeps as a sentinel: nil when it is the sentinel. */
	void WriteOptionalUInt(FMsgPackWriter& Writer, int64 Value, int64 Absent)
	{
		if (Value == Absent)
		{
			Writer.WriteNil();
		}
		else
		{
			Writer.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, Value)));
		}
	}

	TArray<uint8> Framed(TArrayView<const uint8> Body)
	{
		TArray<uint8> Out;
		ApexSpectator::AppendFramed(Out, Body);
		return Out;
	}
}

namespace ApexSpectator
{
	TArray<uint8> EncodeHeader(const FApexStreamHeader& H)
	{
		FMsgPackWriter W(192);
		BeginRecord(W, 14, RecordHeader, H.Epoch);
		W.WriteUInt(H.Version == 0 ? FormatVersion : H.Version);
		W.WriteString(H.StreamId);
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, H.TickRate)));
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, H.FrameRate)));
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, H.RowSize)));

		W.WriteArrayHeader(5);
		W.WriteString(H.Track.TrackId);
		W.WriteString(H.Track.Stem);
		W.WriteString(H.Track.DisplayName);
		W.WriteUInt(H.Track.SourceCrc);
		W.WriteFloat(H.Track.LengthM);

		const FApexSessionConditions& C = H.Conditions;
		W.WriteArrayHeader(6);
		W.WriteUInt(static_cast<uint64>(C.Weather));
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, C.TimeOfDayMinutes)));
		if (C.AirTempC == FApexSessionConditions::AutoAirTemp)
		{
			W.WriteNil();
		}
		else
		{
			W.WriteInt(C.AirTempC);
		}
		WriteOptionalUInt(W, C.HumidityPct, -1);
		WriteOptionalUInt(W, C.WindKph, -1);
		WriteOptionalUInt(W, C.WindFromDeg, -1);

		W.WriteUInt(static_cast<uint64>(H.SessionKind));
		W.WriteUInt(static_cast<uint64>(H.GameMode));
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, H.LapLimit)));

		W.WriteArrayHeader(3);
		WriteOptionalUInt(W, H.RaceStartTick, -1);
		W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, H.StartTick)));
		W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, H.EndTick)));

		W.WriteArrayHeader(2);
		WriteOptionalUInt(W, H.Seed, -1);
		if (H.bHasScore)
		{
			W.WriteFloat(H.Score);
		}
		else
		{
			W.WriteNil();
		}
		return MoveTemp(W.GetBuffer());
	}

	TArray<uint8> EncodeRoster(const FApexStreamRoster& R)
	{
		FMsgPackWriter W(16 + R.Entries.Num() * 72);
		BeginRecord(W, 4, RecordRoster, R.Epoch);
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, R.Revision)));
		W.WriteArrayHeader(R.Entries.Num());
		for (const FApexStreamRosterEntry& E : R.Entries)
		{
			W.WriteArrayHeader(6);
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, E.CarIndex)));
			W.WriteString(E.CarConfigId);
			W.WriteUInt(E.ContentCrc);
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, E.Livery)));
			W.WriteString(E.Name);
			W.WriteBool(E.bIsAi);
		}
		return MoveTemp(W.GetBuffer());
	}

	TArray<uint8> EncodeFrame(const FApexStreamFrame& F)
	{
		FMsgPackWriter W(32 + F.Rows.Num());
		BeginRecord(W, 9, RecordFrame, F.Epoch);
		W.WriteUInt32Fixed(static_cast<uint32>(F.Tick));
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, F.RosterRevision)));
		W.WriteUInt(static_cast<uint64>(F.State));
		W.WriteUInt(F.CountdownMs < 0 ? NoCountdown : static_cast<uint64>(FMath::Min(F.CountdownMs, NoCountdown - 1)));
		W.WriteUInt(static_cast<uint64>(FMath::Max(0, F.Part)));
		W.WriteUInt(static_cast<uint64>(FMath::Max(1, F.Parts)));
		W.WriteBinary(F.Rows);
		return MoveTemp(W.GetBuffer());
	}

	TArray<uint8> EncodeEvent(const FApexStreamEvent& E)
	{
		FMsgPackWriter W(48);
		BeginRecord(W, 5, RecordEvent, E.Epoch);
		W.WriteUInt32Fixed(static_cast<uint32>(E.Tick));
		W.WriteUInt(E.Kind);
		switch (E.Kind)
		{
		case EventLapTiming:
		{
			const FApexLapTiming& T = E.LapTiming;
			W.WriteArrayHeader(8);
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, T.CarIndex)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, T.Lap)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, T.Sector)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, T.SectorTimeMs)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, T.LapTimeMs)));
			W.WriteBool(T.bIsLapEnd);
			W.WriteBool(T.bValid);
			W.WriteUInt((T.bPersonalBestLap ? 1u : 0u) | (T.bSessionBestLap ? 2u : 0u) | (T.bPersonalBestSector ? 4u : 0u)
				| (T.bSessionBestSector ? 8u : 0u));
			break;
		}
		case EventTrackSectors:
			W.WriteArrayHeader(2);
			W.WriteFloat(E.Sectors.TrackLengthM);
			W.WriteArrayHeader(E.Sectors.BoundariesM.Num());
			for (const float Boundary : E.Sectors.BoundariesM)
			{
				W.WriteFloat(Boundary);
			}
			break;
		case EventSessionState:
			W.WriteArrayHeader(1);
			W.WriteUInt(static_cast<uint64>(E.State));
			break;
		case EventFinish:
		case EventRetired:
		case EventPitStop:
			W.WriteArrayHeader(2);
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, E.CarIndex)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, E.Value)));
			break;
		case EventContact:
			W.WriteArrayHeader(3);
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, E.CarIndex)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, E.Other)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, E.SpeedCms)));
			break;
		default:
			return TArray<uint8>();
		}
		return MoveTemp(W.GetBuffer());
	}

	TArray<uint8> EncodePath(const FApexStreamPath& P)
	{
		TArray<uint8> Points;
		Points.Reserve(P.Points.Num() * 8);
		for (const FVector2D& Point : P.Points)
		{
			// Rounded to the millimetre, as `spectator::metres_to_mm`.
			const int32 X = static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(Point.X * 1000.0), static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)));
			const int32 Y = static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(Point.Y * 1000.0), static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)));
			Points.Append(reinterpret_cast<const uint8*>(&X), 4);
			Points.Append(reinterpret_cast<const uint8*>(&Y), 4);
		}
		FMsgPackWriter W(16 + Points.Num());
		BeginRecord(W, 4, RecordPath, P.Epoch);
		W.WriteFloat(P.SpacingM);
		W.WriteBinary(Points);
		return MoveTemp(W.GetBuffer());
	}
}

// --- The file ---------------------------------------------------------------------

void FApexStreamWriter::Add(int64 Tick, TArrayView<const uint8> Body)
{
	if (PendingRecords > 0 && Tick >= PendingFirstTick + BlockTicks)
	{
		Flush();
	}
	if (PendingRecords == 0)
	{
		PendingFirstTick = Tick;
	}
	PendingLastTick = FMath::Max(Tick, PendingFirstTick);
	ApexSpectator::AppendFramed(PendingRaw, Body);
	++PendingRecords;
	++TotalRecords;
}

FApexStreamWriter::FBlock FApexStreamWriter::Compress(int64 FirstTick, int64 LastTick, int32 Records, const TArray<uint8>& Raw)
{
	FBlock Block;
	Block.FirstTick = FirstTick;
	Block.LastTick = LastTick;
	Block.Records = Records;
	Block.RawLen = Raw.Num();
	int32 Size = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
	Block.Zlib.SetNumUninitialized(Size);
	if (FCompression::CompressMemory(NAME_Zlib, Block.Zlib.GetData(), Size, Raw.GetData(), Raw.Num()))
	{
		Block.Zlib.SetNum(Size);
	}
	else
	{
		Block.Zlib.Reset();
		Block.RawLen = 0;
		Block.Records = 0;
	}
	return Block;
}

void FApexStreamWriter::Flush()
{
	if (PendingRecords == 0)
	{
		return;
	}
	FBlock& Block = Blocks.Add_GetRef(Compress(PendingFirstTick, PendingLastTick, PendingRecords, PendingRaw));
	StoredBlockBytes += Block.Zlib.Num();
	PendingRaw.Reset();
	PendingRecords = 0;
}

TArray<uint8> FApexStreamWriter::ToBytes(const FApexStreamHeader& Header, const FApexStreamRoster& Roster,
	const FApexStreamPath* Path, TConstArrayView<FApexStreamEvent> Preamble) const
{
	TArray<uint8> Out;
	Out.Reserve(StoredBytes() + 4096);
	Out.Append(reinterpret_cast<const uint8*>("APXS"), 4);
	Out.Add(static_cast<uint8>(ApexSpectator::FileVersion >> 8));
	Out.Add(static_cast<uint8>(ApexSpectator::FileVersion & 0xFF));
	Out.Append(Framed(ApexSpectator::EncodeHeader(Header)));
	Out.Append(Framed(ApexSpectator::EncodeRoster(Roster)));
	if (Path)
	{
		Out.Append(Framed(ApexSpectator::EncodePath(*Path)));
	}
	for (const FApexStreamEvent& Event : Preamble)
	{
		const TArray<uint8> Body = ApexSpectator::EncodeEvent(Event);
		if (Body.Num() > 0)
		{
			Out.Append(Framed(Body));
		}
	}

	TArray<FApexStreamIndexEntry> Index;
	auto AppendBlock = [&Out, &Index](const FBlock& Block)
	{
		if (Block.Records == 0)
		{
			return;
		}
		FApexStreamIndexEntry& Entry = Index.AddDefaulted_GetRef();
		Entry.FirstTick = Block.FirstTick;
		Entry.Offset = Out.Num();
		Entry.Records = Block.Records;
		FMsgPackWriter W(Block.Zlib.Num() + 32);
		W.WriteArrayHeader(6);
		W.WriteUInt(ApexSpectator::RecordBlock);
		W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, Block.FirstTick)));
		W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, Block.LastTick)));
		W.WriteUInt(static_cast<uint64>(Block.Records));
		W.WriteUInt(static_cast<uint64>(Block.RawLen));
		W.WriteBinary(Block.Zlib);
		Out.Append(Framed(W.GetBuffer()));
	};
	for (const FBlock& Block : Blocks)
	{
		AppendBlock(Block);
	}
	if (PendingRecords > 0)
	{
		AppendBlock(Compress(PendingFirstTick, PendingLastTick, PendingRecords, PendingRaw));
	}

	const uint64 IndexOffset = static_cast<uint64>(Out.Num());
	FMsgPackWriter W(8 + Index.Num() * 16);
	W.WriteArrayHeader(2);
	W.WriteUInt(ApexSpectator::RecordIndex);
	W.WriteArrayHeader(Index.Num());
	for (const FApexStreamIndexEntry& Entry : Index)
	{
		W.WriteArrayHeader(3);
		W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, Entry.FirstTick)));
		W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, Entry.Offset)));
		W.WriteUInt(static_cast<uint64>(Entry.Records));
	}
	Out.Append(Framed(W.GetBuffer()));
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Out.Add(static_cast<uint8>((IndexOffset >> Shift) & 0xFF));
	}
	return Out;
}

void FApexStreamWriter::Reset()
{
	Blocks.Reset();
	PendingRaw.Reset();
	PendingFirstTick = 0;
	PendingLastTick = 0;
	PendingRecords = 0;
	TotalRecords = 0;
	StoredBlockBytes = 0;
}
