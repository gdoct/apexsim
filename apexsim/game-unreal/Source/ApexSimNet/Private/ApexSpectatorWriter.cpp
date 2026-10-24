#include "ApexSpectatorWriter.h"

#include "ApexSimNetModule.h"
#include "Misc/Compression.h"
#include "MsgPack/MsgPackWriter.h"
#include "Tasks/Task.h"

// The physics library (server/ffi) packs a version 2 file's column blocks;
// without it (WITH_APEX_PHYSICS 0) the writer writes version 1.
#include "apexsim_physics.h"

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
		case EventPitService:
		{
			const FApexPitService& P = E.PitService;
			W.WriteArrayHeader(9);
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, P.CarIndex)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, P.PitBox)));
			W.WriteFloat(P.TyresS);
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, P.Compound)));
			W.WriteFloat(P.FuelS);
			W.WriteFloat(P.FuelL);
			W.WriteFloat(P.RepairS);
			W.WriteFloat(P.RepairPct);
			W.WriteFloat(P.TotalS);
			break;
		}
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

	TArray<uint8> EncodeState(const FApexStreamState& S)
	{
		TArray<uint8> Cars;
		Cars.Reserve(S.Cars.Num() * ExtraSize);
		for (const FApexStreamCarExtra& Car : S.Cars)
		{
			Car.Write(Cars);
		}
		FMsgPackWriter W(64 + Cars.Num() + 4 * S.YellowSectors.Num());
		BeginRecord(W, 9, RecordState, S.Epoch);
		W.WriteUInt32Fixed(static_cast<uint32>(S.Tick));
		if (S.Sky.bValid)
		{
			const FApexSkyNow& K = S.Sky;
			W.WriteArrayHeader(19);
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.ClockS, 0, 86399)));
			W.WriteUInt(static_cast<uint64>(K.Weather));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.RainPct, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.CloudPct, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.RoadWaterPct, 0, 255)));
			W.WriteInt(FMath::Clamp(K.AirC, -128, 127));
			W.WriteInt(FMath::Clamp(K.TrackC, -128, 127));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.WindKph, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.WindToDeg, 0, 359)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.LineRubberPct, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.TimeScale, 0, 255)));
			W.WriteUInt(K.NextWeather < 0 ? 255 : static_cast<uint64>(K.NextWeather));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, K.NextInS)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.LineSnowPct, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.SnowfallPct, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.Next2Weather, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, K.Next2InS)));
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(K.Next3Weather, 0, 255)));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, K.Next3InS)));
		}
		else
		{
			W.WriteNil();
		}
		if (S.RaceLeftMs >= 0)
		{
			W.WriteArrayHeader(2);
			W.WriteUInt(static_cast<uint64>(S.RaceLeftMs));
			W.WriteUInt(static_cast<uint64>(FMath::Max(0, S.RaceFinalLap)));
		}
		else
		{
			W.WriteNil();
		}
		W.WriteUInt(ExtraSize);
		W.WriteBinary(Cars);
		if (S.YellowSectors.Num() > 0)
		{
			W.WriteArrayHeader(2);
			W.WriteUInt(static_cast<uint64>(FMath::Clamp(S.YellowSectorM, 0, 65535)));
			W.WriteArrayHeader(S.YellowSectors.Num());
			for (const TPair<int32, int32>& Yellow : S.YellowSectors)
			{
				W.WriteArrayHeader(2);
				W.WriteUInt(static_cast<uint64>(FMath::Clamp(Yellow.Key, 0, 65535)));
				W.WriteUInt(static_cast<uint64>(FMath::Clamp(Yellow.Value, 0, 2)));
			}
		}
		else
		{
			W.WriteNil();
		}
		if (S.RacePhase > 0)
		{
			W.WriteUInt(static_cast<uint64>(FMath::Min(S.RacePhase, 255)));
		}
		else
		{
			W.WriteNil();
		}
		return MoveTemp(W.GetBuffer());
	}

	TArray<uint8> EncodeGhost(const FApexStreamGhost& G)
	{
		TArray<uint8> Samples;
		Samples.SetNumUninitialized(G.Lap.Samples.Num() * 20);
		for (int32 i = 0; i < G.Lap.Samples.Num(); ++i)
		{
			const FApexGhostSample& S = G.Lap.Samples[i];
			const int32 TimeMs = S.TimeMs;
			const float V[4] = {static_cast<float>(S.Position.X), static_cast<float>(S.Position.Y), static_cast<float>(S.Position.Z), S.YawRad};
			FMemory::Memcpy(Samples.GetData() + i * 20, &TimeMs, 4);
			FMemory::Memcpy(Samples.GetData() + i * 20 + 4, V, 16);
		}
		FMsgPackWriter W(64 + Samples.Num() + G.Lap.TrackId.Len() + G.Lap.CarConfigId.Len());
		BeginRecord(W, 8, RecordGhost, G.Epoch);
		W.WriteUInt32Fixed(static_cast<uint32>(G.Tick));
		W.WriteUInt(static_cast<uint64>(FMath::Max(G.Lap.LapTimeMs, 0)));
		W.WriteFloat(G.Lap.SampleHz);
		W.WriteString(G.Lap.TrackId);
		W.WriteString(G.Lap.CarConfigId);
		W.WriteBinary(Samples);
		return MoveTemp(W.GetBuffer());
	}

	TArray<uint8> EncodeRoad(const FApexStreamRoad& R)
	{
		const FApexRoadState& Road = R.Road;
		TArray<uint8> Debris;
		Debris.Reserve(Road.Debris.Num() * 8);
		for (const FVector2D& Piece : Road.Debris)
		{
			const int32 X = static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(Piece.X * 1000.0), static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)));
			const int32 Y = static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(Piece.Y * 1000.0), static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)));
			Debris.Append(reinterpret_cast<const uint8*>(&X), 4);
			Debris.Append(reinterpret_cast<const uint8*>(&Y), 4);
		}
		TArray<uint8> Geometry;
		Geometry.SetNumUninitialized(Road.Geometry.Num() * 16);
		for (int32 i = 0; i < Road.Geometry.Num(); ++i)
		{
			const FVector4f& G = Road.Geometry[i];
			const float V[4] = {G.X, G.Y, G.Z, G.W};
			FMemory::Memcpy(Geometry.GetData() + i * 16, V, 16);
		}
		FMsgPackWriter W(48 + Road.Rows.Num() + Road.SnowRows.Num() + Road.WaterRows.Num() + Debris.Num() + Geometry.Num());
		BeginRecord(W, 13, RecordRoad, R.Epoch);
		W.WriteUInt32Fixed(static_cast<uint32>(R.Tick));
		W.WriteFloat(Road.LapM);
		W.WriteFloat(Road.CellM);
		W.WriteUInt(static_cast<uint64>(FMath::Max(Road.Bins, 0)));
		W.WriteFloat(Road.HalfSpanM);
		W.WriteUInt(static_cast<uint64>(FMath::Max(Road.FirstCell, 0)));
		W.WriteBinary(Road.Rows);
		W.WriteBinary(Debris);
		W.WriteBinary(Geometry);
		W.WriteBinary(Road.SnowRows);
		W.WriteBinary(Road.WaterRows);
		return MoveTemp(W.GetBuffer());
	}
}

// --- The file ---------------------------------------------------------------------

bool FApexStreamWriter::CanPackColumns()
{
#if WITH_APEX_PHYSICS
	static const bool bSameAbi = apx_abi_version() == APX_ABI_VERSION;
	return bSameAbi;
#else
	return false;
#endif
}

FApexStreamWriter::FApexStreamWriter(int32 InTickRate, bool bInColumns)
	: bColumns(bInColumns && CanPackColumns())
{
	BlockTicks = FMath::Max(1, InTickRate) * (bColumns ? ColumnBlockSeconds : 1);
}

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

FApexStreamWriter::FBlock FApexStreamWriter::Pack(int64 FirstTick, int64 LastTick, int32 Records, const TArray<uint8>& Raw, bool bInColumns)
{
	FBlock Block;
	Block.FirstTick = FirstTick;
	Block.LastTick = LastTick;
	Block.Records = Records;
#if WITH_APEX_PHYSICS
	if (bInColumns)
	{
		// A column block is never much larger than its records: one try in all but odd cases.
		TArray<uint8> Body;
		Body.SetNumUninitialized(Raw.Num() + Raw.Num() / 8 + 256);
		size_t Written = 0;
		ApxStatus Status = apx_stream_block_pack(Raw.GetData(), Raw.Num(), static_cast<uint32>(FirstTick), static_cast<uint32>(LastTick),
			Body.GetData(), Body.Num(), &Written);
		if (Status == APX_ERR_BUFFER && Written <= static_cast<size_t>(MAX_int32))
		{
			Body.SetNumUninitialized(static_cast<int32>(Written));
			Status = apx_stream_block_pack(Raw.GetData(), Raw.Num(), static_cast<uint32>(FirstTick), static_cast<uint32>(LastTick),
				Body.GetData(), Body.Num(), &Written);
		}
		if (Status == APX_OK)
		{
			Body.SetNum(static_cast<int32>(Written));
			Block.Body = MoveTemp(Body);
			Block.bColumns = true;
			return Block;
		}
		const char* Why = apx_last_error();
		UE_LOG(LogApexSimNet, Warning, TEXT("Stream block at tick %lld not packed by column (%d: %s); written plain"), FirstTick, Status,
			Why ? UTF8_TO_TCHAR(Why) : TEXT(""));
	}
#endif

	TArray<uint8> Zlib;
	int32 Size = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
	Zlib.SetNumUninitialized(Size);
	if (!FCompression::CompressMemory(NAME_Zlib, Zlib.GetData(), Size, Raw.GetData(), Raw.Num()))
	{
		Block.Records = 0;
		return Block;
	}
	Zlib.SetNum(Size);
	FMsgPackWriter W(Zlib.Num() + 32);
	W.WriteArrayHeader(6);
	W.WriteUInt(ApexSpectator::RecordBlock);
	W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, FirstTick)));
	W.WriteUInt(static_cast<uint64>(FMath::Max<int64>(0, LastTick)));
	W.WriteUInt(static_cast<uint64>(Records));
	W.WriteUInt(static_cast<uint64>(Raw.Num()));
	W.WriteBinary(Zlib);
	Block.Body = MoveTemp(W.GetBuffer());
	return Block;
}

void FApexStreamWriter::Harvest()
{
	int32 Done = 0;
	while (Done < InFlight.Num() && InFlight[Done].IsCompleted())
	{
		FBlock& Block = Blocks.Add_GetRef(MoveTemp(InFlight[Done].GetResult()));
		StoredBlockBytes += Block.Body.Num();
		InFlightRawBytes -= InFlightRaw[Done];
		++Done;
	}
	if (Done > 0)
	{
		InFlight.RemoveAt(0, Done);
		InFlightRaw.RemoveAt(0, Done);
	}
}

void FApexStreamWriter::Flush()
{
	Harvest();
	if (PendingRecords == 0)
	{
		return;
	}
	// Packing ten seconds by column takes milliseconds: not on the game thread.
	InFlightRaw.Add(PendingRaw.Num());
	InFlightRawBytes += PendingRaw.Num();
	InFlight.Add(UE::Tasks::Launch(TEXT("ApexStreamBlockPack"),
		[First = PendingFirstTick, Last = PendingLastTick, Records = PendingRecords, Raw = MoveTemp(PendingRaw), bPackColumns = bColumns]()
		{
			return Pack(First, Last, Records, Raw, bPackColumns);
		}));
	PendingRaw.Reset();
	PendingRecords = 0;
}

TArray<uint8> FApexStreamWriter::ToBytes(const FApexStreamHeader& Header, const FApexStreamRoster& Roster,
	const FApexStreamPath* Path, TConstArrayView<FApexStreamEvent> Preamble) const
{
	// Every block in order: the packed, those still packing (waited for), the open one.
	TArray<const FBlock*> All;
	All.Reserve(Blocks.Num() + InFlight.Num() + 1);
	for (const FBlock& Block : Blocks)
	{
		All.Add(&Block);
	}
	for (UE::Tasks::TTask<FBlock>& Task : InFlight)
	{
		All.Add(&Task.GetResult());
	}
	FBlock Open;
	if (PendingRecords > 0)
	{
		Open = Pack(PendingFirstTick, PendingLastTick, PendingRecords, PendingRaw, bColumns);
		All.Add(&Open);
	}
	const bool bAnyColumns = All.ContainsByPredicate([](const FBlock* Block) { return Block->bColumns && Block->Records > 0; });
	const uint16 Version = bAnyColumns ? 2 : 1;

	TArray<uint8> Out;
	Out.Reserve(StoredBytes() + 4096);
	Out.Append(reinterpret_cast<const uint8*>("APXS"), 4);
	Out.Add(static_cast<uint8>(Version >> 8));
	Out.Add(static_cast<uint8>(Version & 0xFF));
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
	for (const FBlock* Block : All)
	{
		if (Block->Records == 0)
		{
			continue;
		}
		FApexStreamIndexEntry& Entry = Index.AddDefaulted_GetRef();
		Entry.FirstTick = Block->FirstTick;
		Entry.Offset = Out.Num();
		Entry.Records = Block->Records;
		ApexSpectator::AppendFramed(Out, Block->Body);
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
	// A block still packing owns its records: it finishes on its own and is dropped.
	Blocks.Reset();
	InFlight.Reset();
	InFlightRaw.Reset();
	InFlightRawBytes = 0;
	PendingRaw.Reset();
	PendingFirstTick = 0;
	PendingLastTick = 0;
	PendingRecords = 0;
	TotalRecords = 0;
	StoredBlockBytes = 0;
}
