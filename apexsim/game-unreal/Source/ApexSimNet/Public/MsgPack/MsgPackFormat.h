#pragma once

#include "CoreMinimal.h"

/** MessagePack format bytes and the fixed-width tag predicates. */
namespace MsgPack
{
	// Fixed-payload format bytes.
	inline constexpr uint8 Nil     = 0xC0;
	inline constexpr uint8 False   = 0xC2;
	inline constexpr uint8 True    = 0xC3;

	inline constexpr uint8 Bin8    = 0xC4;
	inline constexpr uint8 Bin16   = 0xC5;
	inline constexpr uint8 Bin32   = 0xC6;

	inline constexpr uint8 Ext8    = 0xC7;
	inline constexpr uint8 Ext16   = 0xC8;
	inline constexpr uint8 Ext32   = 0xC9;

	inline constexpr uint8 Float32 = 0xCA;
	inline constexpr uint8 Float64 = 0xCB;

	inline constexpr uint8 UInt8   = 0xCC;
	inline constexpr uint8 UInt16  = 0xCD;
	inline constexpr uint8 UInt32  = 0xCE;
	inline constexpr uint8 UInt64  = 0xCF;

	inline constexpr uint8 Int8    = 0xD0;
	inline constexpr uint8 Int16   = 0xD1;
	inline constexpr uint8 Int32   = 0xD2;
	inline constexpr uint8 Int64   = 0xD3;

	inline constexpr uint8 FixExt1  = 0xD4;
	inline constexpr uint8 FixExt2  = 0xD5;
	inline constexpr uint8 FixExt4  = 0xD6;
	inline constexpr uint8 FixExt8  = 0xD7;
	inline constexpr uint8 FixExt16 = 0xD8;

	inline constexpr uint8 Str8    = 0xD9;
	inline constexpr uint8 Str16   = 0xDA;
	inline constexpr uint8 Str32   = 0xDB;

	inline constexpr uint8 Array16 = 0xDC;
	inline constexpr uint8 Array32 = 0xDD;
	inline constexpr uint8 Map16   = 0xDE;
	inline constexpr uint8 Map32   = 0xDF;

	// Embedded-payload tag ranges.
	inline constexpr uint8 FixMapPrefix   = 0x80;   // 0x80..0x8F, low nibble = pair count
	inline constexpr uint8 FixArrayPrefix = 0x90;   // 0x90..0x9F, low nibble = element count
	inline constexpr uint8 FixStrPrefix   = 0xA0;   // 0xA0..0xBF, low 5 bits = byte length

	inline constexpr int32 MaxFixMapCount   = 15;
	inline constexpr int32 MaxFixArrayCount = 15;
	inline constexpr int32 MaxFixStrLength  = 31;
	inline constexpr int32 MaxPositiveFixInt = 127;

	inline bool IsPositiveFixInt(uint8 Tag) { return Tag <= 0x7F; }
	inline bool IsNegativeFixInt(uint8 Tag) { return Tag >= 0xE0; }
	inline bool IsFixMap(uint8 Tag)         { return (Tag & 0xF0) == FixMapPrefix; }
	inline bool IsFixArray(uint8 Tag)       { return (Tag & 0xF0) == FixArrayPrefix; }
	inline bool IsFixStr(uint8 Tag)         { return (Tag & 0xE0) == FixStrPrefix; }

	inline int32 FixMapCount(uint8 Tag)     { return Tag & 0x0F; }
	inline int32 FixArrayCount(uint8 Tag)   { return Tag & 0x0F; }
	inline int32 FixStrLength(uint8 Tag)    { return Tag & 0x1F; }
}
