#pragma once

#include "CoreMinimal.h"

/**
 * Minimal streaming MessagePack reader.
 *
 * Deliberately a cursor, not a DOM: the server broadcasts `LobbyState` every
 * ~2s carrying every 10th centerline point for all 26 tracks (~14k `{x,y}`
 * maps). Materialising that into a shared-pointer tree twice a minute, for
 * data the menus never read, would be pure waste — so unwanted values are
 * skipped in place with SkipValue().
 *
 * Every Read* returns false and latches an error on malformed or truncated
 * input. Once HasError() is set the cursor is poisoned and all further reads
 * fail, so callers may check once at the end of a parse rather than after
 * every call.
 */
class APEXSIMNET_API FMsgPackReader
{
public:
	explicit FMsgPackReader(TArrayView<const uint8> InData)
		: Data(InData)
	{
	}

	/** Reads a map header, yielding the number of key/value PAIRS that follow. */
	bool ReadMapHeader(int32& OutCount);
	/** Reads an array header, yielding the number of elements that follow. */
	bool ReadArrayHeader(int32& OutCount);
	/** Reads a str value as UTF-8. bin is not accepted (the server never sends it for strings). */
	bool ReadString(FString& Out);
	bool ReadUInt64(uint64& Out);
	bool ReadInt64(int64& Out);
	/** Accepts float32, float64 and any integer type — the server is free to emit 0 as an int. */
	bool ReadFloat(float& Out);
	bool ReadBool(bool& Out);

	/** Consumes a nil and returns true; leaves the cursor untouched and returns false otherwise. */
	bool TryReadNil();

	/**
	 * Reads a str, or consumes a nil and yields an empty string.
	 * `Option<String>` fields (LobbyPlayer::SelectedCar/InSession) need this.
	 */
	bool ReadStringOrNil(FString& Out);

	/** Skips exactly one complete value (recursing through containers). */
	bool SkipValue();

	bool IsAtEnd() const { return Pos >= Data.Num(); }
	bool HasError() const { return bHasError; }
	const FString& GetError() const { return Error; }
	int32 Tell() const { return Pos; }
	int32 Remaining() const { return Data.Num() - Pos; }

private:
	static constexpr int32 MaxSkipDepth = 32;

	/** Ensures N more bytes are readable; latches an error and returns false if not. */
	bool Need(int32 N);
	bool ReadByte(uint8& Out);
	bool PeekByte(uint8& Out) const;
	/** Reads N big-endian bytes into a uint64. */
	bool ReadBigEndian(int32 NumBytes, uint64& Out);
	/** Reads a container/string length given a format byte, or reports it is not that kind. */
	bool ReadLength(int32 NumLengthBytes, int32& OutLength);
	bool SkipValueInternal(int32 Depth);
	bool Fail(const FString& Reason);

	TArrayView<const uint8> Data;
	int32 Pos = 0;
	bool bHasError = false;
	FString Error;
};
