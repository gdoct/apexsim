#include "MsgPack/MsgPackWriter.h"

#include "MsgPack/MsgPackFormat.h"

void FMsgPackWriter::PushBigEndian(uint64 Value, int32 NumBytes)
{
	for (int32 i = NumBytes - 1; i >= 0; --i)
	{
		Buffer.Add(static_cast<uint8>((Value >> (i * 8)) & 0xFF));
	}
}

void FMsgPackWriter::PushBytes(const uint8* Bytes, int32 Count)
{
	Buffer.Append(Bytes, Count);
}

void FMsgPackWriter::WriteMapHeader(int32 PairCount)
{
	check(PairCount >= 0);
	if (PairCount <= MsgPack::MaxFixMapCount)
	{
		PushByte(static_cast<uint8>(MsgPack::FixMapPrefix | PairCount));
	}
	else if (PairCount <= MAX_uint16)
	{
		PushByte(MsgPack::Map16);
		PushBigEndian(static_cast<uint64>(PairCount), 2);
	}
	else
	{
		PushByte(MsgPack::Map32);
		PushBigEndian(static_cast<uint64>(PairCount), 4);
	}
}

void FMsgPackWriter::WriteArrayHeader(int32 ElementCount)
{
	check(ElementCount >= 0);
	if (ElementCount <= MsgPack::MaxFixArrayCount)
	{
		PushByte(static_cast<uint8>(MsgPack::FixArrayPrefix | ElementCount));
	}
	else if (ElementCount <= MAX_uint16)
	{
		PushByte(MsgPack::Array16);
		PushBigEndian(static_cast<uint64>(ElementCount), 2);
	}
	else
	{
		PushByte(MsgPack::Array32);
		PushBigEndian(static_cast<uint64>(ElementCount), 4);
	}
}

void FMsgPackWriter::WriteStringBytes(const ANSICHAR* Utf8Bytes, int32 ByteLength)
{
	if (ByteLength <= MsgPack::MaxFixStrLength)
	{
		PushByte(static_cast<uint8>(MsgPack::FixStrPrefix | ByteLength));
	}
	else if (ByteLength <= MAX_uint8)
	{
		PushByte(MsgPack::Str8);
		PushBigEndian(static_cast<uint64>(ByteLength), 1);
	}
	else if (ByteLength <= MAX_uint16)
	{
		PushByte(MsgPack::Str16);
		PushBigEndian(static_cast<uint64>(ByteLength), 2);
	}
	else
	{
		PushByte(MsgPack::Str32);
		PushBigEndian(static_cast<uint64>(ByteLength), 4);
	}
	PushBytes(reinterpret_cast<const uint8*>(Utf8Bytes), ByteLength);
}

void FMsgPackWriter::WriteString(const FString& Value)
{
	FTCHARToUTF8 Converter(*Value);
	WriteStringBytes(Converter.Get(), Converter.Length());
}

void FMsgPackWriter::WriteString(const ANSICHAR* Value)
{
	WriteStringBytes(Value, static_cast<int32>(FCStringAnsi::Strlen(Value)));
}

void FMsgPackWriter::WriteUInt(uint64 Value)
{
	if (Value <= static_cast<uint64>(MsgPack::MaxPositiveFixInt))
	{
		PushByte(static_cast<uint8>(Value));
	}
	else if (Value <= MAX_uint8)
	{
		PushByte(MsgPack::UInt8);
		PushBigEndian(Value, 1);
	}
	else if (Value <= MAX_uint16)
	{
		PushByte(MsgPack::UInt16);
		PushBigEndian(Value, 2);
	}
	else if (Value <= MAX_uint32)
	{
		PushByte(MsgPack::UInt32);
		PushBigEndian(Value, 4);
	}
	else
	{
		PushByte(MsgPack::UInt64);
		PushBigEndian(Value, 8);
	}
}

void FMsgPackWriter::WriteInt(int64 Value)
{
	if (Value >= 0)
	{
		WriteUInt(static_cast<uint64>(Value));
		return;
	}

	if (Value >= -32)
	{
		// Negative fixint: 0xE0..0xFF holds -32..-1 directly in the tag byte.
		PushByte(static_cast<uint8>(static_cast<int8>(Value)));
	}
	else if (Value >= MIN_int8)
	{
		PushByte(MsgPack::Int8);
		PushBigEndian(static_cast<uint64>(static_cast<uint8>(static_cast<int8>(Value))), 1);
	}
	else if (Value >= MIN_int16)
	{
		PushByte(MsgPack::Int16);
		PushBigEndian(static_cast<uint64>(static_cast<uint16>(static_cast<int16>(Value))), 2);
	}
	else if (Value >= MIN_int32)
	{
		PushByte(MsgPack::Int32);
		PushBigEndian(static_cast<uint64>(static_cast<uint32>(static_cast<int32>(Value))), 4);
	}
	else
	{
		PushByte(MsgPack::Int64);
		PushBigEndian(static_cast<uint64>(Value), 8);
	}
}

void FMsgPackWriter::WriteFloat(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(uint32));
	PushByte(MsgPack::Float32);
	PushBigEndian(static_cast<uint64>(Bits), 4);
}

void FMsgPackWriter::WriteBool(bool Value)
{
	PushByte(Value ? MsgPack::True : MsgPack::False);
}

void FMsgPackWriter::WriteNil()
{
	PushByte(MsgPack::Nil);
}
