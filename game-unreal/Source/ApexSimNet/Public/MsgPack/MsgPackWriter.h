#pragma once

#include "CoreMinimal.h"

/**
 * Minimal MessagePack writer covering exactly what the client sends.
 *
 * Always picks the narrowest encoding for a value, matching `rmp_serde`'s
 * behaviour — the golden-bytes encode tests in ProtocolCodecTests depend on
 * that, and byte-exactness is the cheapest way to prove the client agrees with
 * the server about the wire format.
 */
class APEXSIMNET_API FMsgPackWriter
{
public:
	FMsgPackWriter() = default;
	explicit FMsgPackWriter(int32 ReserveBytes) { Buffer.Reserve(ReserveBytes); }

	void WriteMapHeader(int32 PairCount);
	void WriteArrayHeader(int32 ElementCount);

	void WriteString(const FString& Value);
	/** ASCII key literals — skips the UTF-8 conversion entirely. */
	void WriteString(const ANSICHAR* Value);

	void WriteUInt(uint64 Value);
	void WriteInt(int64 Value);
	void WriteFloat(float Value);
	void WriteBool(bool Value);
	void WriteNil();

	const TArray<uint8>& GetBuffer() const { return Buffer; }
	TArray<uint8>& GetBuffer() { return Buffer; }
	int32 Num() const { return Buffer.Num(); }
	void Reset() { Buffer.Reset(); }

private:
	void PushByte(uint8 Byte) { Buffer.Add(Byte); }
	/** Appends Value as NumBytes big-endian bytes. */
	void PushBigEndian(uint64 Value, int32 NumBytes);
	void PushBytes(const uint8* Bytes, int32 Count);
	void WriteStringBytes(const ANSICHAR* Utf8Bytes, int32 ByteLength);

	TArray<uint8> Buffer;
};
