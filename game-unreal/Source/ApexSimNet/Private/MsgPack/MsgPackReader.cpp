#include "MsgPack/MsgPackReader.h"

#include "MsgPack/MsgPackFormat.h"

bool FMsgPackReader::Fail(const FString& Reason)
{
	if (!bHasError)
	{
		bHasError = true;
		Error = FString::Printf(TEXT("%s (at offset %d of %d)"), *Reason, Pos, Data.Num());
	}
	return false;
}

bool FMsgPackReader::Need(int32 N)
{
	if (bHasError)
	{
		return false;
	}
	if (N < 0 || Pos > Data.Num() - N)
	{
		return Fail(FString::Printf(TEXT("truncated: need %d more byte(s)"), N));
	}
	return true;
}

bool FMsgPackReader::ReadByte(uint8& Out)
{
	if (!Need(1))
	{
		return false;
	}
	Out = Data[Pos++];
	return true;
}

bool FMsgPackReader::PeekByte(uint8& Out) const
{
	if (bHasError || Pos >= Data.Num())
	{
		return false;
	}
	Out = Data[Pos];
	return true;
}

bool FMsgPackReader::ReadBigEndian(int32 NumBytes, uint64& Out)
{
	if (!Need(NumBytes))
	{
		return false;
	}
	uint64 Value = 0;
	for (int32 i = 0; i < NumBytes; ++i)
	{
		Value = (Value << 8) | static_cast<uint64>(Data[Pos + i]);
	}
	Pos += NumBytes;
	Out = Value;
	return true;
}

bool FMsgPackReader::ReadLength(int32 NumLengthBytes, int32& OutLength)
{
	uint64 Raw = 0;
	if (!ReadBigEndian(NumLengthBytes, Raw))
	{
		return false;
	}
	if (Raw > static_cast<uint64>(MAX_int32))
	{
		return Fail(TEXT("length exceeds int32"));
	}
	OutLength = static_cast<int32>(Raw);
	return true;
}

bool FMsgPackReader::ReadMapHeader(int32& OutCount)
{
	uint8 Tag = 0;
	if (!ReadByte(Tag))
	{
		return false;
	}
	if (MsgPack::IsFixMap(Tag))
	{
		OutCount = MsgPack::FixMapCount(Tag);
		return true;
	}
	switch (Tag)
	{
	case MsgPack::Map16:
		return ReadLength(2, OutCount);
	case MsgPack::Map32:
		return ReadLength(4, OutCount);
	default:
		return Fail(FString::Printf(TEXT("expected map, got format byte 0x%02X"), Tag));
	}
}

bool FMsgPackReader::ReadArrayHeader(int32& OutCount)
{
	uint8 Tag = 0;
	if (!ReadByte(Tag))
	{
		return false;
	}
	if (MsgPack::IsFixArray(Tag))
	{
		OutCount = MsgPack::FixArrayCount(Tag);
		return true;
	}
	switch (Tag)
	{
	case MsgPack::Array16:
		return ReadLength(2, OutCount);
	case MsgPack::Array32:
		return ReadLength(4, OutCount);
	default:
		return Fail(FString::Printf(TEXT("expected array, got format byte 0x%02X"), Tag));
	}
}

bool FMsgPackReader::ReadString(FString& Out)
{
	uint8 Tag = 0;
	if (!ReadByte(Tag))
	{
		return false;
	}

	int32 Length = 0;
	if (MsgPack::IsFixStr(Tag))
	{
		Length = MsgPack::FixStrLength(Tag);
	}
	else
	{
		switch (Tag)
		{
		case MsgPack::Str8:
			if (!ReadLength(1, Length)) { return false; }
			break;
		case MsgPack::Str16:
			if (!ReadLength(2, Length)) { return false; }
			break;
		case MsgPack::Str32:
			if (!ReadLength(4, Length)) { return false; }
			break;
		default:
			return Fail(FString::Printf(TEXT("expected str, got format byte 0x%02X"), Tag));
		}
	}

	if (!Need(Length))
	{
		return false;
	}

	// FUTF8ToTCHAR wants a NUL-terminated-or-explicitly-sized buffer; the
	// sized overload is exact and avoids copying into a temporary.
	Out = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Data.GetData() + Pos), Length));
	Pos += Length;
	return true;
}

bool FMsgPackReader::ReadStringOrNil(FString& Out)
{
	if (TryReadNil())
	{
		Out.Reset();
		return true;
	}
	return ReadString(Out);
}

bool FMsgPackReader::TryReadNil()
{
	uint8 Tag = 0;
	if (!PeekByte(Tag) || Tag != MsgPack::Nil)
	{
		return false;
	}
	++Pos;
	return true;
}

bool FMsgPackReader::ReadUInt64(uint64& Out)
{
	uint8 Tag = 0;
	if (!ReadByte(Tag))
	{
		return false;
	}
	if (MsgPack::IsPositiveFixInt(Tag))
	{
		Out = Tag;
		return true;
	}
	switch (Tag)
	{
	case MsgPack::UInt8:  return ReadBigEndian(1, Out);
	case MsgPack::UInt16: return ReadBigEndian(2, Out);
	case MsgPack::UInt32: return ReadBigEndian(4, Out);
	case MsgPack::UInt64: return ReadBigEndian(8, Out);
	default:
		break;
	}

	// A signed encoding is acceptable so long as the value is non-negative;
	// rmp picks the narrowest form, which for small values is a fixint.
	--Pos;
	int64 Signed = 0;
	if (!ReadInt64(Signed))
	{
		return false;
	}
	if (Signed < 0)
	{
		return Fail(TEXT("expected unsigned integer, got a negative value"));
	}
	Out = static_cast<uint64>(Signed);
	return true;
}

bool FMsgPackReader::ReadInt64(int64& Out)
{
	uint8 Tag = 0;
	if (!ReadByte(Tag))
	{
		return false;
	}
	if (MsgPack::IsPositiveFixInt(Tag))
	{
		Out = Tag;
		return true;
	}
	if (MsgPack::IsNegativeFixInt(Tag))
	{
		Out = static_cast<int8>(Tag);
		return true;
	}

	uint64 Raw = 0;
	switch (Tag)
	{
	case MsgPack::UInt8:
		if (!ReadBigEndian(1, Raw)) { return false; }
		Out = static_cast<int64>(Raw);
		return true;
	case MsgPack::UInt16:
		if (!ReadBigEndian(2, Raw)) { return false; }
		Out = static_cast<int64>(Raw);
		return true;
	case MsgPack::UInt32:
		if (!ReadBigEndian(4, Raw)) { return false; }
		Out = static_cast<int64>(Raw);
		return true;
	case MsgPack::UInt64:
		if (!ReadBigEndian(8, Raw)) { return false; }
		if (Raw > static_cast<uint64>(MAX_int64))
		{
			return Fail(TEXT("uint64 value does not fit in int64"));
		}
		Out = static_cast<int64>(Raw);
		return true;
	case MsgPack::Int8:
		if (!ReadBigEndian(1, Raw)) { return false; }
		Out = static_cast<int8>(static_cast<uint8>(Raw));
		return true;
	case MsgPack::Int16:
		if (!ReadBigEndian(2, Raw)) { return false; }
		Out = static_cast<int16>(static_cast<uint16>(Raw));
		return true;
	case MsgPack::Int32:
		if (!ReadBigEndian(4, Raw)) { return false; }
		Out = static_cast<int32>(static_cast<uint32>(Raw));
		return true;
	case MsgPack::Int64:
		if (!ReadBigEndian(8, Raw)) { return false; }
		Out = static_cast<int64>(Raw);
		return true;
	default:
		return Fail(FString::Printf(TEXT("expected integer, got format byte 0x%02X"), Tag));
	}
}

bool FMsgPackReader::ReadFloat(float& Out)
{
	uint8 Tag = 0;
	if (!PeekByte(Tag))
	{
		return Need(1);
	}

	if (Tag == MsgPack::Float32)
	{
		++Pos;
		uint64 Raw = 0;
		if (!ReadBigEndian(4, Raw))
		{
			return false;
		}
		const uint32 Bits = static_cast<uint32>(Raw);
		// Bit-cast, not memcpy of the wire bytes: ReadBigEndian has already
		// undone the wire's big-endian order into host integer order.
		float Value;
		FMemory::Memcpy(&Value, &Bits, sizeof(float));
		Out = Value;
		return true;
	}

	if (Tag == MsgPack::Float64)
	{
		++Pos;
		uint64 Raw = 0;
		if (!ReadBigEndian(8, Raw))
		{
			return false;
		}
		double Value;
		FMemory::Memcpy(&Value, &Raw, sizeof(double));
		Out = static_cast<float>(Value);
		return true;
	}

	// Integers are acceptable: rmp encodes a whole-valued f32 field as-is, but
	// a hand-rolled or future producer may narrow it.
	int64 AsInt = 0;
	if (!ReadInt64(AsInt))
	{
		return false;
	}
	Out = static_cast<float>(AsInt);
	return true;
}

bool FMsgPackReader::ReadBool(bool& Out)
{
	uint8 Tag = 0;
	if (!ReadByte(Tag))
	{
		return false;
	}
	if (Tag == MsgPack::True)
	{
		Out = true;
		return true;
	}
	if (Tag == MsgPack::False)
	{
		Out = false;
		return true;
	}
	return Fail(FString::Printf(TEXT("expected bool, got format byte 0x%02X"), Tag));
}

bool FMsgPackReader::SkipValue()
{
	return SkipValueInternal(0);
}

bool FMsgPackReader::SkipValueInternal(int32 Depth)
{
	if (Depth > MaxSkipDepth)
	{
		return Fail(TEXT("nesting depth limit exceeded while skipping"));
	}

	uint8 Tag = 0;
	if (!ReadByte(Tag))
	{
		return false;
	}

	if (MsgPack::IsPositiveFixInt(Tag) || MsgPack::IsNegativeFixInt(Tag))
	{
		return true;
	}

	if (MsgPack::IsFixStr(Tag))
	{
		const int32 Length = MsgPack::FixStrLength(Tag);
		if (!Need(Length)) { return false; }
		Pos += Length;
		return true;
	}

	if (MsgPack::IsFixMap(Tag))
	{
		const int32 Count = MsgPack::FixMapCount(Tag);
		for (int32 i = 0; i < Count; ++i)
		{
			if (!SkipValueInternal(Depth + 1) || !SkipValueInternal(Depth + 1)) { return false; }
		}
		return true;
	}

	if (MsgPack::IsFixArray(Tag))
	{
		const int32 Count = MsgPack::FixArrayCount(Tag);
		for (int32 i = 0; i < Count; ++i)
		{
			if (!SkipValueInternal(Depth + 1)) { return false; }
		}
		return true;
	}

	int32 Length = 0;
	int32 Count = 0;

	switch (Tag)
	{
	case MsgPack::Nil:
	case MsgPack::True:
	case MsgPack::False:
		return true;

	case MsgPack::UInt8:
	case MsgPack::Int8:
		if (!Need(1)) { return false; }
		Pos += 1;
		return true;

	case MsgPack::UInt16:
	case MsgPack::Int16:
		if (!Need(2)) { return false; }
		Pos += 2;
		return true;

	case MsgPack::UInt32:
	case MsgPack::Int32:
	case MsgPack::Float32:
		if (!Need(4)) { return false; }
		Pos += 4;
		return true;

	case MsgPack::UInt64:
	case MsgPack::Int64:
	case MsgPack::Float64:
		if (!Need(8)) { return false; }
		Pos += 8;
		return true;

	// bin is never produced by `to_vec_named` for our types, but skipping it
	// correctly is what keeps the cursor in sync if the server ever adds one.
	case MsgPack::Str8:
	case MsgPack::Bin8:
		if (!ReadLength(1, Length) || !Need(Length)) { return false; }
		Pos += Length;
		return true;

	case MsgPack::Str16:
	case MsgPack::Bin16:
		if (!ReadLength(2, Length) || !Need(Length)) { return false; }
		Pos += Length;
		return true;

	case MsgPack::Str32:
	case MsgPack::Bin32:
		if (!ReadLength(4, Length) || !Need(Length)) { return false; }
		Pos += Length;
		return true;

	case MsgPack::Array16:
		if (!ReadLength(2, Count)) { return false; }
		for (int32 i = 0; i < Count; ++i)
		{
			if (!SkipValueInternal(Depth + 1)) { return false; }
		}
		return true;

	case MsgPack::Array32:
		if (!ReadLength(4, Count)) { return false; }
		for (int32 i = 0; i < Count; ++i)
		{
			if (!SkipValueInternal(Depth + 1)) { return false; }
		}
		return true;

	case MsgPack::Map16:
		if (!ReadLength(2, Count)) { return false; }
		for (int32 i = 0; i < Count; ++i)
		{
			if (!SkipValueInternal(Depth + 1) || !SkipValueInternal(Depth + 1)) { return false; }
		}
		return true;

	case MsgPack::Map32:
		if (!ReadLength(4, Count)) { return false; }
		for (int32 i = 0; i < Count; ++i)
		{
			if (!SkipValueInternal(Depth + 1) || !SkipValueInternal(Depth + 1)) { return false; }
		}
		return true;

	case MsgPack::FixExt1:  if (!Need(2))  { return false; } Pos += 2;  return true;
	case MsgPack::FixExt2:  if (!Need(3))  { return false; } Pos += 3;  return true;
	case MsgPack::FixExt4:  if (!Need(5))  { return false; } Pos += 5;  return true;
	case MsgPack::FixExt8:  if (!Need(9))  { return false; } Pos += 9;  return true;
	case MsgPack::FixExt16: if (!Need(17)) { return false; } Pos += 17; return true;

	case MsgPack::Ext8:
		if (!ReadLength(1, Length) || !Need(Length + 1)) { return false; }
		Pos += Length + 1;
		return true;

	case MsgPack::Ext16:
		if (!ReadLength(2, Length) || !Need(Length + 1)) { return false; }
		Pos += Length + 1;
		return true;

	case MsgPack::Ext32:
		if (!ReadLength(4, Length) || !Need(Length + 1)) { return false; }
		Pos += Length + 1;
		return true;

	default:
		return Fail(FString::Printf(TEXT("unknown format byte 0x%02X"), Tag));
	}
}
