#include "ApexSpectatorStream.h"

#include "ApexSimNetModule.h"
#include "Misc/Compression.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "MsgPack/MsgPackFormat.h"
#include "MsgPack/MsgPackReader.h"

// Named, not anonymous: this module is unity-built.
namespace ApexSpectatorCodec
{
	constexpr int64 MaxInflatedBytes = 64ll << 20;

	/** Skip whatever a newer writer appended to an array this build knows `Known` of. */
	bool SkipRest(FMsgPackReader& Reader, int32 Count, int32 Known)
	{
		for (int32 i = Known; i < Count; ++i)
		{
			if (!Reader.SkipValue())
			{
				return false;
			}
		}
		return true;
	}

	bool ReadU32(FMsgPackReader& Reader, uint32& Out)
	{
		uint64 Raw = 0;
		if (!Reader.ReadUInt64(Raw))
		{
			return false;
		}
		Out = static_cast<uint32>(Raw);
		return true;
	}

	bool ReadI32(FMsgPackReader& Reader, int32& Out)
	{
		int64 Raw = 0;
		if (!Reader.ReadInt64(Raw))
		{
			return false;
		}
		Out = static_cast<int32>(Raw);
		return true;
	}

	bool ReadI64(FMsgPackReader& Reader, int64& Out)
	{
		return Reader.ReadInt64(Out);
	}

	/** A nil or an integer: `Absent` when nil. */
	bool ReadOptionalI64(FMsgPackReader& Reader, int64& Out, int64 Absent = -1)
	{
		if (Reader.TryReadNil())
		{
			Out = Absent;
			return true;
		}
		return Reader.ReadInt64(Out);
	}

	bool ParseHeader(FMsgPackReader& Reader, int32 Count, uint32 Epoch, FApexStreamHeader& Out)
	{
		if (Count < 14)
		{
			return false;
		}
		Out.Epoch = Epoch;
		int32 Version = 0;
		if (!ReadI32(Reader, Version))
		{
			return false;
		}
		Out.Version = static_cast<uint8>(Version);
		if (Out.Version != ApexSpectator::FormatVersion)
		{
			return false;
		}
		if (!Reader.ReadString(Out.StreamId) || !ReadI32(Reader, Out.TickRate) || !ReadI32(Reader, Out.FrameRate)
			|| !ReadI32(Reader, Out.RowSize))
		{
			return false;
		}

		int32 TrackCount = 0;
		if (!Reader.ReadArrayHeader(TrackCount) || TrackCount < 5)
		{
			return false;
		}
		if (!Reader.ReadString(Out.Track.TrackId) || !Reader.ReadString(Out.Track.Stem)
			|| !Reader.ReadString(Out.Track.DisplayName) || !ReadU32(Reader, Out.Track.SourceCrc)
			|| !Reader.ReadFloat(Out.Track.LengthM) || !SkipRest(Reader, TrackCount, 5))
		{
			return false;
		}

		int32 ConditionCount = 0;
		if (!Reader.ReadArrayHeader(ConditionCount) || ConditionCount < 6)
		{
			return false;
		}
		int32 Weather = 0;
		int32 Minutes = 0;
		if (!ReadI32(Reader, Weather) || !ReadI32(Reader, Minutes))
		{
			return false;
		}
		Out.Conditions.Weather = static_cast<EApexWeather>(FMath::Clamp(Weather, 0, 4));
		Out.Conditions.TimeOfDayMinutes = Minutes;
		int64 Value = 0;
		if (!ReadOptionalI64(Reader, Value, FApexSessionConditions::AutoAirTemp)) { return false; }
		Out.Conditions.AirTempC = static_cast<int32>(Value);
		if (!ReadOptionalI64(Reader, Value)) { return false; }
		Out.Conditions.HumidityPct = static_cast<int32>(Value);
		if (!ReadOptionalI64(Reader, Value)) { return false; }
		Out.Conditions.WindKph = static_cast<int32>(Value);
		if (!ReadOptionalI64(Reader, Value)) { return false; }
		Out.Conditions.WindFromDeg = static_cast<int32>(Value);
		if (!SkipRest(Reader, ConditionCount, 6))
		{
			return false;
		}

		int32 Kind = 0;
		int32 Mode = 0;
		if (!ReadI32(Reader, Kind) || !ReadI32(Reader, Mode) || !ReadI32(Reader, Out.LapLimit))
		{
			return false;
		}
		Out.SessionKind = static_cast<EApexSessionKind>(FMath::Clamp(Kind, 0, 4));
		Out.GameMode = static_cast<EApexGameMode>(FMath::Clamp(Mode, 0, 8));

		int32 TickCount = 0;
		if (!Reader.ReadArrayHeader(TickCount) || TickCount < 3)
		{
			return false;
		}
		if (!ReadOptionalI64(Reader, Out.RaceStartTick) || !ReadI64(Reader, Out.StartTick)
			|| !ReadI64(Reader, Out.EndTick) || !SkipRest(Reader, TickCount, 3))
		{
			return false;
		}

		int32 RenderCount = 0;
		if (!Reader.ReadArrayHeader(RenderCount) || RenderCount < 2)
		{
			return false;
		}
		if (!ReadOptionalI64(Reader, Out.Seed))
		{
			return false;
		}
		if (Reader.TryReadNil())
		{
			Out.bHasScore = false;
		}
		else if (Reader.ReadFloat(Out.Score))
		{
			Out.bHasScore = true;
		}
		else
		{
			return false;
		}
		return SkipRest(Reader, RenderCount, 2) && SkipRest(Reader, Count, 14);
	}

	bool ParseRoster(FMsgPackReader& Reader, int32 Count, uint32 Epoch, FApexStreamRoster& Out)
	{
		if (Count < 4)
		{
			return false;
		}
		Out.Epoch = Epoch;
		if (!ReadI32(Reader, Out.Revision))
		{
			return false;
		}
		int32 Entries = 0;
		if (!Reader.ReadArrayHeader(Entries))
		{
			return false;
		}
		Out.Entries.Reset(Entries);
		for (int32 i = 0; i < Entries; ++i)
		{
			int32 Fields = 0;
			if (!Reader.ReadArrayHeader(Fields) || Fields < 6)
			{
				return false;
			}
			FApexStreamRosterEntry& Entry = Out.Entries.AddDefaulted_GetRef();
			if (!ReadI32(Reader, Entry.CarIndex) || !Reader.ReadString(Entry.CarConfigId)
				|| !ReadU32(Reader, Entry.ContentCrc) || !ReadI32(Reader, Entry.Livery)
				|| !Reader.ReadString(Entry.Name) || !Reader.ReadBool(Entry.bIsAi) || !SkipRest(Reader, Fields, 6))
			{
				return false;
			}
		}
		return SkipRest(Reader, Count, 4);
	}

	bool ParseFrame(FMsgPackReader& Reader, int32 Count, uint32 Epoch, FApexStreamFrame& Out)
	{
		if (Count < 9)
		{
			return false;
		}
		Out.Epoch = Epoch;
		int32 State = 0;
		int32 Countdown = 0;
		if (!ReadI64(Reader, Out.Tick) || !ReadI32(Reader, Out.RosterRevision) || !ReadI32(Reader, State)
			|| !ReadI32(Reader, Countdown) || !ReadI32(Reader, Out.Part) || !ReadI32(Reader, Out.Parts))
		{
			return false;
		}
		Out.State = static_cast<EApexSessionState>(FMath::Clamp(State, 0, 3));
		Out.CountdownMs = Countdown == ApexSpectator::NoCountdown ? -1 : Countdown;
		TArrayView<const uint8> Rows;
		if (!Reader.ReadBinary(Rows))
		{
			return false;
		}
		Out.Rows = TArray<uint8>(Rows.GetData(), Rows.Num());
		return SkipRest(Reader, Count, 9);
	}

	bool ParseEvent(FMsgPackReader& Reader, int32 Count, uint32 Epoch, FApexStreamEvent& Out)
	{
		if (Count < 5)
		{
			return false;
		}
		Out.Epoch = Epoch;
		int32 Kind = 0;
		if (!ReadI64(Reader, Out.Tick) || !ReadI32(Reader, Kind))
		{
			return false;
		}
		Out.Kind = static_cast<uint8>(Kind);
		int32 Fields = 0;
		if (!Reader.ReadArrayHeader(Fields))
		{
			return false;
		}
		int32 Known = 0;
		switch (Out.Kind)
		{
		case ApexSpectator::EventLapTiming:
		{
			if (Fields < 8)
			{
				return false;
			}
			int32 Flags = 0;
			FApexLapTiming& T = Out.LapTiming;
			if (!ReadI32(Reader, T.CarIndex) || !ReadI32(Reader, T.Lap) || !ReadI32(Reader, T.Sector)
				|| !ReadI32(Reader, T.SectorTimeMs) || !ReadI32(Reader, T.LapTimeMs) || !Reader.ReadBool(T.bIsLapEnd)
				|| !Reader.ReadBool(T.bValid) || !ReadI32(Reader, Flags))
			{
				return false;
			}
			T.bPersonalBestLap = (Flags & 1) != 0;
			T.bSessionBestLap = (Flags & 2) != 0;
			T.bPersonalBestSector = (Flags & 4) != 0;
			T.bSessionBestSector = (Flags & 8) != 0;
			Known = 8;
			break;
		}
		case ApexSpectator::EventTrackSectors:
		{
			if (Fields < 2 || !Reader.ReadFloat(Out.Sectors.TrackLengthM))
			{
				return false;
			}
			int32 Boundaries = 0;
			if (!Reader.ReadArrayHeader(Boundaries))
			{
				return false;
			}
			Out.Sectors.BoundariesM.Reset(Boundaries);
			for (int32 i = 0; i < Boundaries; ++i)
			{
				float B = 0.0f;
				if (!Reader.ReadFloat(B))
				{
					return false;
				}
				Out.Sectors.BoundariesM.Add(B);
			}
			Known = 2;
			break;
		}
		case ApexSpectator::EventSessionState:
		{
			int32 State = 0;
			if (Fields < 1 || !ReadI32(Reader, State))
			{
				return false;
			}
			Out.State = static_cast<EApexSessionState>(FMath::Clamp(State, 0, 3));
			Known = 1;
			break;
		}
		case ApexSpectator::EventFinish:
		case ApexSpectator::EventRetired:
		case ApexSpectator::EventPitStop:
			if (Fields < 2 || !ReadI32(Reader, Out.CarIndex) || !ReadI32(Reader, Out.Value))
			{
				return false;
			}
			Known = 2;
			break;
		case ApexSpectator::EventContact:
			if (Fields < 3 || !ReadI32(Reader, Out.CarIndex) || !ReadI32(Reader, Out.Other) || !ReadI32(Reader, Out.SpeedCms))
			{
				return false;
			}
			Known = 3;
			break;
		default:
			break;
		}
		return SkipRest(Reader, Fields, Known) && SkipRest(Reader, Count, 5);
	}

	bool ParsePath(FMsgPackReader& Reader, int32 Count, uint32 Epoch, FApexStreamPath& Out)
	{
		if (Count < 4)
		{
			return false;
		}
		Out.Epoch = Epoch;
		TArrayView<const uint8> Bytes;
		if (!Reader.ReadFloat(Out.SpacingM) || !Reader.ReadBinary(Bytes))
		{
			return false;
		}
		const int32 Points = Bytes.Num() / 8;
		Out.Points.Reset(Points);
		for (int32 i = 0; i < Points; ++i)
		{
			const uint8* P = Bytes.GetData() + i * 8;
			int32 X = 0;
			int32 Y = 0;
			FMemory::Memcpy(&X, P, 4);
			FMemory::Memcpy(&Y, P + 4, 4);
			Out.Points.Add(FVector2D(X / 1000.0, Y / 1000.0));
		}
		return SkipRest(Reader, Count, 4);
	}

	bool ParseBlock(FMsgPackReader& Reader, int32 Count, FApexStreamBlock& Out)
	{
		if (Count < 6)
		{
			return false;
		}
		TArrayView<const uint8> Zlib;
		if (!ReadI64(Reader, Out.FirstTick) || !ReadI64(Reader, Out.LastTick) || !ReadI32(Reader, Out.Records)
			|| !ReadI32(Reader, Out.RawLen) || !Reader.ReadBinary(Zlib))
		{
			return false;
		}
		Out.Zlib = TArray<uint8>(Zlib.GetData(), Zlib.Num());
		return SkipRest(Reader, Count, 6);
	}

	bool ParseIndex(FMsgPackReader& Reader, int32 Count, TArray<FApexStreamIndexEntry>& Out)
	{
		if (Count < 2)
		{
			return false;
		}
		int32 Entries = 0;
		if (!Reader.ReadArrayHeader(Entries))
		{
			return false;
		}
		Out.Reset(Entries);
		for (int32 i = 0; i < Entries; ++i)
		{
			int32 Fields = 0;
			if (!Reader.ReadArrayHeader(Fields) || Fields < 3)
			{
				return false;
			}
			FApexStreamIndexEntry& Entry = Out.AddDefaulted_GetRef();
			if (!ReadI64(Reader, Entry.FirstTick) || !ReadI64(Reader, Entry.Offset) || !ReadI32(Reader, Entry.Records)
				|| !SkipRest(Reader, Fields, 3))
			{
				return false;
			}
		}
		return SkipRest(Reader, Count, 2);
	}

	uint32 BigEndian32(const uint8* P)
	{
		return (static_cast<uint32>(P[0]) << 24) | (static_cast<uint32>(P[1]) << 16) | (static_cast<uint32>(P[2]) << 8) | P[3];
	}
}

// --- Rows ---------------------------------------------------------------------------

bool FApexStreamCarRow::Read(const uint8* B, int32 Len, FApexStreamCarRow& Out)
{
	if (Len < ApexSpectator::RowSizeV1)
	{
		return false;
	}
	auto I32 = [B](int32 O) { int32 V; FMemory::Memcpy(&V, B + O, 4); return V; };
	auto U16 = [B](int32 O) { uint16 V; FMemory::Memcpy(&V, B + O, 2); return V; };
	Out.CarIndex = B[0];
	Out.Status = B[1];
	Out.XMm = I32(2);
	Out.YMm = I32(6);
	Out.ZMm = I32(10);
	Out.Yaw = U16(14);
	Out.Pitch = static_cast<int16>(U16(16));
	Out.Roll = static_cast<int16>(U16(18));
	Out.SpeedCms = U16(20);
	Out.Steering = static_cast<int8>(B[22]);
	Out.Throttle = B[23];
	Out.Brake = B[24];
	Out.Gear = static_cast<int8>(B[25]);
	Out.EngineRpm = U16(26);
	Out.Lap = U16(28);
	Out.StationCm = static_cast<uint32>(I32(30));
	Out.FinishPosition = B[34];
	Out.LapFlags = B[35];
	Out.PitFlags = B[36];
	Out.Compound = B[37];
	for (int32 i = 0; i < 5; ++i)
	{
		Out.Damage[i] = B[38 + i];
	}
	Out.ErsFlags = B[43];
	// Version 1 rows end here: their tyres stay unknown.
	const bool bTyres = Len >= ApexSpectator::RowSize;
	for (int32 i = 0; i < 4; ++i)
	{
		Out.TyreWear[i] = bTyres ? B[44 + i] : ApexSpectator::TyreWearUnknown;
		Out.TyreC[i] = bTyres ? B[48 + i] : 0;
	}
	return true;
}

void FApexStreamCarRow::Write(TArray<uint8>& Out) const
{
	const int32 Start = Out.AddUninitialized(ApexSpectator::RowSize);
	uint8* B = Out.GetData() + Start;
	auto PutI32 = [B](int32 O, int32 V) { FMemory::Memcpy(B + O, &V, 4); };
	auto PutU16 = [B](int32 O, uint16 V) { FMemory::Memcpy(B + O, &V, 2); };
	B[0] = static_cast<uint8>(CarIndex);
	B[1] = Status;
	PutI32(2, XMm);
	PutI32(6, YMm);
	PutI32(10, ZMm);
	PutU16(14, Yaw);
	PutU16(16, static_cast<uint16>(Pitch));
	PutU16(18, static_cast<uint16>(Roll));
	PutU16(20, SpeedCms);
	B[22] = static_cast<uint8>(Steering);
	B[23] = Throttle;
	B[24] = Brake;
	B[25] = static_cast<uint8>(Gear);
	PutU16(26, EngineRpm);
	PutU16(28, Lap);
	PutI32(30, static_cast<int32>(StationCm));
	B[34] = FinishPosition;
	B[35] = LapFlags;
	B[36] = PitFlags;
	B[37] = Compound;
	for (int32 i = 0; i < 5; ++i)
	{
		B[38 + i] = Damage[i];
	}
	B[43] = ErsFlags;
	for (int32 i = 0; i < 4; ++i)
	{
		B[44 + i] = TyreWear[i];
		B[48 + i] = TyreC[i];
	}
}

FApexCarTelemetry FApexStreamCarRow::ToTelemetry() const
{
	FApexCarTelemetry T;
	T.CarIndex = CarIndex;
	T.Position = FVector(XMm / 1000.0, YMm / 1000.0, ZMm / 1000.0);
	T.YawRad = Yaw * (UE_TWO_PI / 65536.0f);
	T.PitchRad = Pitch * (UE_PI / 32768.0f);
	T.RollRad = Roll * (UE_PI / 32768.0f);
	T.SpeedMps = SpeedCms / 100.0f;
	T.Throttle = Throttle / 255.0f;
	T.Brake = Brake / 255.0f;
	T.Steering = Steering / 127.0f;
	T.Gear = Gear;
	T.EngineRpm = EngineRpm;
	T.CurrentLap = Lap;
	T.TrackProgress = StationCm / 100.0f;
	T.FinishPosition = FinishPosition;
	T.bIsOnTrack = (Status & ApexSpectator::StatusOnTrack) != 0;
	T.bIsColliding = (Status & ApexSpectator::StatusColliding) != 0;
	T.bLapInvalid = (LapFlags & 1) != 0;
	T.bLastLapInvalid = (LapFlags & 2) != 0;
	T.bInGarage = (LapFlags & 4) != 0 || (Status & ApexSpectator::StatusInGarage) != 0;
	T.bDrsAllowed = (LapFlags & 8) != 0;
	T.bDrsOpen = (LapFlags & 16) != 0;
	T.bHeadlights = (LapFlags & 32) != 0;
	T.bHeadlightFlash = (LapFlags & 64) != 0;
	T.bPitLimiter = (PitFlags & 1) != 0;
	T.bPitServicing = (PitFlags & 2) != 0;
	T.bInPitLane = (PitFlags & 4) != 0;
	T.bPitAutopilot = (PitFlags & 8) != 0;
	T.bPitExitClosed = (PitFlags & 16) != 0;
	T.bPitHeld = (PitFlags & 32) != 0;
	T.Compound = Compound == 255 ? -1 : Compound;
	for (int32 i = 0; i < 5; ++i)
	{
		T.DamagePct[i] = Damage[i];
	}
	T.ErsMode = ErsFlags & 3;
	T.bErsDeploying = (ErsFlags & 4) != 0;
	T.bErsHarvesting = (ErsFlags & 8) != 0;
	T.bErsBoost = (ErsFlags & 16) != 0;
	for (int32 i = 0; i < 4; ++i)
	{
		T.TyreWearPct[i] = TyreWear[i] == ApexSpectator::TyreWearUnknown ? -1.0f : TyreWear[i];
		T.TyreTempC[i] = TyreC[i] == 0 ? -1.0f : TyreC[i];
	}
	return T;
}

FApexStreamCarRow FApexStreamCarRow::FromTelemetry(const FApexCarTelemetry& T)
{
	auto Unit = [](float V) { return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(V, 0.0f, 1.0f) * 255.0f)); };
	auto Mm = [](double Metres) { return static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(Metres * 1000.0), static_cast<double>(MIN_int32), static_cast<double>(MAX_int32))); };
	auto Angle = [](float Rad) { return static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Rad / UE_PI * 32768.0f), -32768, 32767)); };

	FApexStreamCarRow Row;
	Row.CarIndex = T.CarIndex;
	bool bRetired = false;
	if (T.HasDamage())
	{
		for (const float Pct : T.DamagePct)
		{
			bRetired = bRetired || Pct >= 100.0f;
		}
	}
	Row.Status = (T.bIsOnTrack ? ApexSpectator::StatusOnTrack : 0) | (T.bIsColliding ? ApexSpectator::StatusColliding : 0)
		| (T.bInGarage ? ApexSpectator::StatusInGarage : 0) | (bRetired ? ApexSpectator::StatusRetired : 0)
		| (T.FinishPosition > 0 ? ApexSpectator::StatusFinished : 0);
	Row.XMm = Mm(T.Position.X);
	Row.YMm = Mm(T.Position.Y);
	Row.ZMm = Mm(T.Position.Z);
	const double Turns = FMath::Fmod(static_cast<double>(T.YawRad) / UE_DOUBLE_TWO_PI, 1.0);
	Row.Yaw = static_cast<uint16>(FMath::RoundToInt((Turns < 0.0 ? Turns + 1.0 : Turns) * 65536.0) & 0xFFFF);
	Row.Pitch = Angle(T.PitchRad);
	Row.Roll = Angle(T.RollRad);
	Row.SpeedCms = static_cast<uint16>(FMath::Min(FMath::RoundToInt(FMath::Abs(T.SpeedMps) * 100.0f), 65535));
	Row.Steering = static_cast<int8>(FMath::RoundToInt(FMath::Clamp(T.Steering, -1.0f, 1.0f) * 127.0f));
	Row.Throttle = Unit(T.Throttle);
	Row.Brake = Unit(T.Brake);
	Row.Gear = static_cast<int8>(FMath::Clamp(T.Gear, -1, 127));
	Row.EngineRpm = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(T.EngineRpm), 0, 65535));
	Row.Lap = static_cast<uint16>(FMath::Clamp(T.CurrentLap, 0, 65535));
	Row.StationCm = static_cast<uint32>(FMath::RoundToDouble(FMath::Max(0.0, static_cast<double>(T.TrackProgress)) * 100.0));
	Row.FinishPosition = static_cast<uint8>(FMath::Clamp(T.FinishPosition, 0, 255));
	Row.LapFlags = (T.bLapInvalid ? 1 : 0) | (T.bLastLapInvalid ? 2 : 0) | (T.bInGarage ? 4 : 0) | (T.bDrsAllowed ? 8 : 0)
		| (T.bDrsOpen ? 16 : 0) | (T.bHeadlights ? 32 : 0) | (T.bHeadlightFlash ? 64 : 0);
	Row.PitFlags = (T.bPitLimiter ? 1 : 0) | (T.bPitServicing ? 2 : 0) | (T.bInPitLane ? 4 : 0)
		| (T.bPitAutopilot ? 8 : 0) | (T.bPitExitClosed ? 16 : 0) | (T.bPitHeld ? 32 : 0);
	Row.Compound = T.Compound < 0 ? 255 : static_cast<uint8>(FMath::Min(T.Compound, 254));
	for (int32 i = 0; i < 5; ++i)
	{
		Row.Damage[i] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(T.DamagePct[i]), 0, 100));
	}
	Row.ErsFlags = (T.ErsMode >= 0 ? static_cast<uint8>(T.ErsMode & 3) : 0) | (T.bErsDeploying ? 4 : 0)
		| (T.bErsHarvesting ? 8 : 0) | (T.bErsBoost ? 16 : 0);
	for (int32 i = 0; i < 4; ++i)
	{
		Row.TyreWear[i] = T.TyreWearPct[i] < 0.0f ? ApexSpectator::TyreWearUnknown
			: static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(T.TyreWearPct[i]), 0, 100));
		Row.TyreC[i] = T.TyreTempC[i] < 0.0f ? 0 : static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(T.TyreTempC[i]), 1, 255));
	}
	return Row;
}

TArray<FApexStreamCarRow> FApexStreamFrame::Cars(int32 RowSize) const
{
	const int32 Stride = FMath::Max(RowSize, ApexSpectator::RowSizeV1);
	TArray<FApexStreamCarRow> Out;
	Out.Reserve(Rows.Num() / Stride);
	for (int32 Offset = 0; Offset + Stride <= Rows.Num(); Offset += Stride)
	{
		FApexStreamCarRow Row;
		if (FApexStreamCarRow::Read(Rows.GetData() + Offset, Stride, Row))
		{
			Out.Add(Row);
		}
	}
	return Out;
}

FApexSessionRoster FApexStreamRoster::ToSessionRoster(const FString& SessionId) const
{
	FApexSessionRoster Out;
	Out.SessionId = SessionId;
	Out.Entries.Reserve(Entries.Num());
	for (const FApexStreamRosterEntry& Entry : Entries)
	{
		FApexRosterEntry& Row = Out.Entries.AddDefaulted_GetRef();
		Row.CarIndex = Entry.CarIndex;
		// A stream names no players; the index stands in, so each entry is distinct.
		Row.PlayerId = FString::Printf(TEXT("stream-%d"), Entry.CarIndex);
		Row.PlayerName = Entry.Name;
		Row.bIsAi = Entry.bIsAi;
		Row.CarConfigId = Entry.CarConfigId;
		Row.Livery = Entry.Livery;
	}
	return Out;
}

// --- Records ----------------------------------------------------------------------------

namespace ApexSpectator
{
	uint8 RecordType(TArrayView<const uint8> Body)
	{
		if (Body.Num() < 2 || !MsgPack::IsFixArray(Body[0]) || Body[1] >= 0x80)
		{
			return 0;
		}
		return Body[1];
	}

	bool RecordEpoch(TArrayView<const uint8> Body, uint32& OutEpoch)
	{
		if (Body.Num() < 7 || Body[2] != MsgPack::UInt32)
		{
			return false;
		}
		OutEpoch = ApexSpectatorCodec::BigEndian32(Body.GetData() + 3);
		return true;
	}

	bool RecordTick(TArrayView<const uint8> Body, int64& OutTick)
	{
		const uint8 Type = RecordType(Body);
		if ((Type != RecordFrame && Type != RecordEvent) || Body.Num() < 12 || Body[2] != MsgPack::UInt32
			|| Body[7] != MsgPack::UInt32)
		{
			return false;
		}
		OutTick = ApexSpectatorCodec::BigEndian32(Body.GetData() + 8);
		return true;
	}

	bool IsRecordDatagram(TArrayView<const uint8> Datagram)
	{
		return RecordType(Datagram) != 0;
	}

	bool DecodeRecord(TArrayView<const uint8> Body, FApexStreamRecord& Out, FString& OutError)
	{
		using namespace ApexSpectatorCodec;
		Out = FApexStreamRecord();
		FMsgPackReader Reader(Body);
		int32 Count = 0;
		if (!Reader.ReadArrayHeader(Count) || Count < 2)
		{
			OutError = Reader.HasError() ? Reader.GetError() : TEXT("a record is an array of at least two");
			return false;
		}
		int32 Type = 0;
		if (!ReadI32(Reader, Type))
		{
			OutError = Reader.GetError();
			return false;
		}
		Out.Type = static_cast<uint8>(Type);
		bool bOk = true;
		switch (Out.Type)
		{
		case RecordBlock:
			bOk = ParseBlock(Reader, Count, Out.Block);
			break;
		case RecordIndex:
			bOk = ParseIndex(Reader, Count, Out.Index);
			break;
		case RecordHeader:
		case RecordRoster:
		case RecordFrame:
		case RecordEvent:
		case RecordPath:
		{
			uint32 Epoch = 0;
			if (!ReadU32(Reader, Epoch))
			{
				bOk = false;
				break;
			}
			switch (Out.Type)
			{
			case RecordHeader: bOk = ParseHeader(Reader, Count, Epoch, Out.Header); break;
			case RecordRoster: bOk = ParseRoster(Reader, Count, Epoch, Out.Roster); break;
			case RecordFrame:  bOk = ParseFrame(Reader, Count, Epoch, Out.Frame); break;
			case RecordEvent:  bOk = ParseEvent(Reader, Count, Epoch, Out.Event); break;
			default:           bOk = ParsePath(Reader, Count, Epoch, Out.Path); break;
			}
			break;
		}
		default:
			// A record type this build does not know: its type is enough.
			break;
		}
		if (!bOk || Reader.HasError())
		{
			OutError = Reader.HasError() ? Reader.GetError()
				: FString::Printf(TEXT("record type %d does not decode"), Out.Type);
			return false;
		}
		return true;
	}

	bool SplitFramed(TArrayView<const uint8> Run, TArray<TArrayView<const uint8>>& OutBodies, FString& OutError)
	{
		OutBodies.Reset();
		int32 Pos = 0;
		while (Pos < Run.Num())
		{
			if (Pos + 4 > Run.Num())
			{
				OutError = TEXT("truncated record length");
				return false;
			}
			const int32 Len = static_cast<int32>(ApexSpectatorCodec::BigEndian32(Run.GetData() + Pos));
			if (Len < 0 || Pos + 4 + Len > Run.Num())
			{
				OutError = TEXT("truncated record");
				return false;
			}
			OutBodies.Add(TArrayView<const uint8>(Run.GetData() + Pos + 4, Len));
			Pos += 4 + Len;
		}
		return true;
	}

	void AppendFramed(TArray<uint8>& Out, TArrayView<const uint8> Body)
	{
		const uint32 Len = static_cast<uint32>(Body.Num());
		Out.Add(static_cast<uint8>(Len >> 24));
		Out.Add(static_cast<uint8>(Len >> 16));
		Out.Add(static_cast<uint8>(Len >> 8));
		Out.Add(static_cast<uint8>(Len));
		Out.Append(Body.GetData(), Body.Num());
	}
}

// --- The file -------------------------------------------------------------------------------

bool FApexStreamFile::IsStreamFile(const FString& FilePath)
{
	TArray<uint8> Head;
	// The whole file: a stream is a few megabytes and this is asked once per file.
	if (!FFileHelper::LoadFileToArray(Head, *FilePath))
	{
		return false;
	}
	return Head.Num() >= 4 && Head[0] == 'A' && Head[1] == 'P' && Head[2] == 'X' && Head[3] == 'S';
}

bool FApexStreamFile::LoadFromFile(const FString& FilePath, FString& OutError)
{
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *FilePath))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *FilePath);
		return false;
	}
	return LoadFromBytes(MoveTemp(Data), OutError);
}

bool FApexStreamFile::ReadPreamble(const FString& FilePath, FApexStreamHeader& OutHeader, FApexStreamRoster& OutRoster, FString& OutError)
{
	// The preamble is a few kilobytes (the path record the largest, under
	// 20 KB on the longest circuit); a megabyte is more than enough.
	constexpr int64 PeekBytes = 1 << 20;
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
	if (!Reader)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *FilePath);
		return false;
	}
	TArray<uint8> Head;
	Head.SetNumUninitialized(static_cast<int32>(FMath::Min(Reader->TotalSize(), PeekBytes)));
	Reader->Serialize(Head.GetData(), Head.Num());
	Reader->Close();
	if (Head.Num() < 6 || Head[0] != 'A' || Head[1] != 'P' || Head[2] != 'X' || Head[3] != 'S')
	{
		OutError = TEXT("no APXS magic");
		return false;
	}
	bool bHeader = false;
	bool bRoster = false;
	int64 Offset = 6;
	while (Offset + 4 <= Head.Num())
	{
		const int64 Len = ApexSpectatorCodec::BigEndian32(Head.GetData() + Offset);
		if (Offset + 4 + Len > Head.Num())
		{
			break;
		}
		const TArrayView<const uint8> Body(Head.GetData() + Offset + 4, static_cast<int32>(Len));
		const uint8 Type = ApexSpectator::RecordType(Body);
		if (Type == ApexSpectator::RecordBlock || Type == ApexSpectator::RecordIndex || (bHeader && bRoster))
		{
			break;
		}
		FApexStreamRecord Record;
		if (!ApexSpectator::DecodeRecord(Body, Record, OutError))
		{
			return false;
		}
		if (Record.Type == ApexSpectator::RecordHeader)
		{
			OutHeader = MoveTemp(Record.Header);
			bHeader = true;
		}
		else if (Record.Type == ApexSpectator::RecordRoster)
		{
			OutRoster = MoveTemp(Record.Roster);
			bRoster = true;
		}
		Offset += 4 + Len;
	}
	if (!bHeader || !bRoster)
	{
		OutError = TEXT("no header or roster at the start of the file");
		return false;
	}
	return true;
}

bool FApexStreamFile::ReadRecordAt(int64 Offset, TArrayView<const uint8>& OutBody, int64& OutNext, FString& OutError) const
{
	const int64 Trailer = Bytes.Num() - 8;
	if (Offset < 0 || Offset + 4 > Trailer)
	{
		OutError = TEXT("record past the end of the file");
		return false;
	}
	const int64 Len = ApexSpectatorCodec::BigEndian32(Bytes.GetData() + Offset);
	if (Offset + 4 + Len > Trailer)
	{
		OutError = TEXT("record past the end of the file");
		return false;
	}
	OutBody = TArrayView<const uint8>(Bytes.GetData() + Offset + 4, static_cast<int32>(Len));
	OutNext = Offset + 4 + Len;
	return true;
}

bool FApexStreamFile::LoadFromBytes(TArray<uint8>&& Data, FString& OutError)
{
	Bytes = MoveTemp(Data);
	Preamble.Reset();
	Index.Reset();
	bHasPath = false;
	if (Bytes.Num() < 14 || Bytes[0] != 'A' || Bytes[1] != 'P' || Bytes[2] != 'X' || Bytes[3] != 'S')
	{
		OutError = TEXT("no APXS magic");
		return false;
	}
	const uint16 Version = (static_cast<uint16>(Bytes[4]) << 8) | Bytes[5];
	if (Version != ApexSpectator::FileVersion)
	{
		OutError = FString::Printf(TEXT("file version %d (this build reads %d)"), Version, ApexSpectator::FileVersion);
		return false;
	}
	const int64 Trailer = Bytes.Num() - 8;
	int64 IndexOffset = 0;
	for (int32 i = 0; i < 8; ++i)
	{
		IndexOffset = (IndexOffset << 8) | Bytes[Trailer + i];
	}
	TArrayView<const uint8> Body;
	int64 Next = 0;
	FApexStreamRecord Record;
	if (!ReadRecordAt(IndexOffset, Body, Next, OutError) || !ApexSpectator::DecodeRecord(Body, Record, OutError))
	{
		return false;
	}
	if (Record.Type != ApexSpectator::RecordIndex)
	{
		OutError = TEXT("the trailer does not point at the index");
		return false;
	}
	Index = MoveTemp(Record.Index);

	bool bHeader = false;
	bool bRoster = false;
	int64 Offset = 6;
	PreambleEnd = 6;
	while (Offset < IndexOffset)
	{
		if (!ReadRecordAt(Offset, Body, Next, OutError))
		{
			return false;
		}
		if (ApexSpectator::RecordType(Body) == ApexSpectator::RecordBlock)
		{
			break;
		}
		PreambleEnd = Next;
		if (!ApexSpectator::DecodeRecord(Body, Record, OutError))
		{
			return false;
		}
		switch (Record.Type)
		{
		case ApexSpectator::RecordHeader: Header = MoveTemp(Record.Header); bHeader = true; break;
		case ApexSpectator::RecordRoster: Roster = MoveTemp(Record.Roster); bRoster = true; break;
		case ApexSpectator::RecordPath:   Path = MoveTemp(Record.Path); bHasPath = true; break;
		case ApexSpectator::RecordEvent:  Preamble.Add(MoveTemp(Record.Event)); break;
		default: break;
		}
		Offset = Next;
	}
	if (!bHeader || !bRoster)
	{
		OutError = TEXT("no header or roster ahead of the blocks");
		return false;
	}
	return true;
}

bool FApexStreamFile::ReadBlock(int32 BlockIndex, TArray<TArray<uint8>>& OutBodies, FString& OutError) const
{
	OutBodies.Reset();
	if (!Index.IsValidIndex(BlockIndex))
	{
		OutError = FString::Printf(TEXT("no block %d"), BlockIndex);
		return false;
	}
	TArrayView<const uint8> Body;
	int64 Next = 0;
	FApexStreamRecord Record;
	if (!ReadRecordAt(Index[BlockIndex].Offset, Body, Next, OutError) || !ApexSpectator::DecodeRecord(Body, Record, OutError))
	{
		return false;
	}
	if (Record.Type != ApexSpectator::RecordBlock)
	{
		OutError = TEXT("the index does not point at a block");
		return false;
	}
	const FApexStreamBlock& Block = Record.Block;
	if (Block.RawLen < 0 || Block.RawLen > ApexSpectatorCodec::MaxInflatedBytes)
	{
		OutError = TEXT("a block claims an absurd size");
		return false;
	}
	TArray<uint8> Raw;
	Raw.SetNumUninitialized(Block.RawLen);
	if (Block.RawLen > 0
		&& !FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), Block.RawLen, Block.Zlib.GetData(), Block.Zlib.Num()))
	{
		OutError = FString::Printf(TEXT("block %d does not inflate"), BlockIndex);
		return false;
	}
	TArray<TArrayView<const uint8>> Views;
	if (!ApexSpectator::SplitFramed(Raw, Views, OutError))
	{
		return false;
	}
	OutBodies.Reserve(Views.Num());
	for (const TArrayView<const uint8>& View : Views)
	{
		OutBodies.Add(TArray<uint8>(View.GetData(), View.Num()));
	}
	return true;
}

int32 FApexStreamFile::BlockForTick(int64 Tick) const
{
	int32 Found = 0;
	for (int32 i = 0; i < Index.Num(); ++i)
	{
		if (Index[i].FirstTick <= Tick)
		{
			Found = i;
		}
	}
	return Found;
}

bool FApexStreamFile::ReadAll(TArray<TArray<uint8>>& OutBodies, FString& OutError) const
{
	OutBodies.Reset();
	for (int32 i = 0; i < Index.Num(); ++i)
	{
		TArray<TArray<uint8>> Block;
		if (!ReadBlock(i, Block, OutError))
		{
			return false;
		}
		OutBodies.Append(MoveTemp(Block));
	}
	return true;
}

// --- The player --------------------------------------------------------------------------------

void FApexSpectatorPlayer::Reset()
{
	*this = FApexSpectatorPlayer();
}

bool FApexSpectatorPlayer::ApplyFramed(TArrayView<const uint8> Run, FString& OutError)
{
	TArray<TArrayView<const uint8>> Bodies;
	if (!ApexSpectator::SplitFramed(Run, Bodies, OutError))
	{
		return false;
	}
	bool bOk = true;
	for (const TArrayView<const uint8>& Body : Bodies)
	{
		bOk &= Apply(Body, OutError);
	}
	return bOk;
}

bool FApexSpectatorPlayer::Apply(TArrayView<const uint8> Body, FString& OutError)
{
	FApexStreamRecord Record;
	if (!ApexSpectator::DecodeRecord(Body, Record, OutError))
	{
		return false;
	}
	switch (Record.Type)
	{
	case ApexSpectator::RecordHeader:
		// A header starts an epoch: everything of the one before is stale,
		// a roster of the new one follows.
		Flush();
		Header = MoveTemp(Record.Header);
		bHasHeader = true;
		bHeaderChanged = true;
		bHasRoster = false;
		bHasPath = false;
		bPending = false;
		Frames.Reset();
		Events.Reset();
		return true;
	case ApexSpectator::RecordRoster:
		if (!bHasHeader || Record.Roster.Epoch != Header.Epoch)
		{
			return true;
		}
		Flush();
		Roster = MoveTemp(Record.Roster);
		bHasRoster = true;
		bRosterChanged = true;
		return true;
	case ApexSpectator::RecordPath:
		if (bHasHeader && Record.Path.Epoch == Header.Epoch)
		{
			Path = MoveTemp(Record.Path);
			bHasPath = true;
		}
		return true;
	case ApexSpectator::RecordEvent:
		if (bHasHeader && Record.Event.Epoch == Header.Epoch)
		{
			Events.Add(MoveTemp(Record.Event));
		}
		return true;
	case ApexSpectator::RecordFrame:
		if (!bHasHeader || !bHasRoster || Record.Frame.Epoch != Header.Epoch || Record.Frame.RosterRevision != Roster.Revision)
		{
			++DroppedFrames;
			return true;
		}
		AcceptFrame(MoveTemp(Record.Frame));
		return true;
	default:
		return true;
	}
}

void FApexSpectatorPlayer::AcceptFrame(FApexStreamFrame&& Frame)
{
	if (Frame.Parts <= 1)
	{
		Flush();
		Pending = MoveTemp(Frame);
		PendingMask = 1;
		bPending = true;
		Flush();
		return;
	}
	if (bPending && Pending.Tick != Frame.Tick)
	{
		// A newer tick: whatever parts the one before got is what it is.
		Flush();
	}
	if (!bPending)
	{
		Pending = Frame;
		Pending.Rows.Reset();
		PendingMask = 0;
		bPending = true;
	}
	const uint32 Bit = Frame.Part < 32 ? (1u << Frame.Part) : 0u;
	if (PendingMask & Bit)
	{
		return;
	}
	PendingMask |= Bit;
	Pending.Rows.Append(Frame.Rows);
	const uint32 All = Frame.Parts >= 32 ? ~0u : (1u << Frame.Parts) - 1u;
	if ((PendingMask & All) == All)
	{
		Flush();
	}
}

void FApexSpectatorPlayer::Flush()
{
	if (!bPending)
	{
		return;
	}
	bPending = false;
	const uint32 All = Pending.Parts >= 32 ? ~0u : (1u << FMath::Max(Pending.Parts, 1)) - 1u;
	if ((PendingMask & All) != All)
	{
		++IncompleteFrames;
	}
	FApexTelemetryFrame& Out = Frames.AddDefaulted_GetRef();
	Out.ServerTick = Pending.Tick;
	Out.SessionState = Pending.State;
	Out.GameMode = Header.GameMode;
	Out.CountdownMs = Pending.CountdownMs;
	TArray<FApexStreamCarRow> Rows = Pending.Cars(Header.RowSize);
	Rows.Sort([](const FApexStreamCarRow& A, const FApexStreamCarRow& B) { return A.CarIndex < B.CarIndex; });
	Out.Cars.Reserve(Rows.Num());
	for (const FApexStreamCarRow& Row : Rows)
	{
		Out.Cars.Add(Row.ToTelemetry());
	}
	++AppliedFrames;
}

TArray<FApexTelemetryFrame> FApexSpectatorPlayer::TakeFrames()
{
	TArray<FApexTelemetryFrame> Out = MoveTemp(Frames);
	Frames.Reset();
	return Out;
}

TArray<FApexStreamEvent> FApexSpectatorPlayer::TakeEvents()
{
	TArray<FApexStreamEvent> Out = MoveTemp(Events);
	Events.Reset();
	return Out;
}
