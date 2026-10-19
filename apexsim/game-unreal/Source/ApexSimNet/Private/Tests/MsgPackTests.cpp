#include "Misc/AutomationTest.h"

#include "MsgPack/MsgPackFormat.h"
#include "MsgPack/MsgPackReader.h"
#include "MsgPack/MsgPackWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags ApexTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

	FMsgPackReader MakeReader(const FMsgPackWriter& Writer)
	{
		return FMsgPackReader(TArrayView<const uint8>(Writer.GetBuffer()));
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexMsgPackRoundTripTest,
	"ApexSim.Net.MsgPack.RoundTrip",
	ApexTestFlags)

bool FApexMsgPackRoundTripTest::RunTest(const FString& Parameters)
{
	// Boundary widths are where a narrowest-encoding writer changes format
	// byte, so they are where an encoding bug hides.
	const TArray<uint64> UnsignedCases = {
		0, 1, 126, 127, 128, 254, 255, 256, 65534, 65535, 65536,
		4294967294ull, 4294967295ull, 4294967296ull, MAX_uint64
	};
	for (uint64 Value : UnsignedCases)
	{
		FMsgPackWriter Writer;
		Writer.WriteUInt(Value);
		FMsgPackReader Reader = MakeReader(Writer);
		uint64 Decoded = 0;
		TestTrue(FString::Printf(TEXT("read uint %llu"), Value), Reader.ReadUInt64(Decoded));
		TestEqual(FString::Printf(TEXT("uint %llu round-trips"), Value), Decoded, Value);
		TestTrue(FString::Printf(TEXT("uint %llu consumed fully"), Value), Reader.IsAtEnd());
	}

	const TArray<int64> SignedCases = {
		0, -1, -31, -32, -33, -127, -128, -129,
		-32767, -32768, -32769, -2147483647LL, -2147483648LL, -2147483649LL,
		127, 128, 32767, 32768
	};
	for (int64 Value : SignedCases)
	{
		FMsgPackWriter Writer;
		Writer.WriteInt(Value);
		FMsgPackReader Reader = MakeReader(Writer);
		int64 Decoded = 0;
		TestTrue(FString::Printf(TEXT("read int %lld"), Value), Reader.ReadInt64(Decoded));
		TestEqual(FString::Printf(TEXT("int %lld round-trips"), Value), Decoded, Value);
		TestTrue(FString::Printf(TEXT("int %lld consumed fully"), Value), Reader.IsAtEnd());
	}

	const TArray<float> FloatCases = {
		0.0f, -0.0f, 1.0f, -1.5f, 798.0f, 12000.5f, 3.4028235e38f, 1.17549435e-38f
	};
	for (float Value : FloatCases)
	{
		FMsgPackWriter Writer;
		Writer.WriteFloat(Value);
		FMsgPackReader Reader = MakeReader(Writer);
		float Decoded = 0.0f;
		TestTrue(TEXT("read float"), Reader.ReadFloat(Decoded));
		TestEqual(FString::Printf(TEXT("float %f round-trips exactly"), Value), Decoded, Value);
	}

	// 31 vs 32 characters straddles fixstr -> str8; 255 vs 256 straddles
	// str8 -> str16.
	const TArray<FString> StringCases = {
		TEXT(""),
		TEXT("a"),
		FString::ChrN(31, TEXT('x')),
		FString::ChrN(32, TEXT('x')),
		FString::ChrN(255, TEXT('y')),
		FString::ChrN(256, TEXT('y')),
		FString::ChrN(70000, TEXT('z')),
		// Real ApexSim track names — these are why UTF-8 matters here.
		TEXT("Nürburgring"),
		TEXT("São Paulo"),
		TEXT("Circuit of The Americas"),
	};
	for (const FString& Value : StringCases)
	{
		FMsgPackWriter Writer;
		Writer.WriteString(Value);
		FMsgPackReader Reader = MakeReader(Writer);
		FString Decoded;
		TestTrue(TEXT("read string"), Reader.ReadString(Decoded));
		TestEqual(TEXT("string round-trips"), Decoded, Value);
		TestTrue(TEXT("string consumed fully"), Reader.IsAtEnd());
	}

	{
		FMsgPackWriter Writer;
		Writer.WriteBool(true);
		Writer.WriteBool(false);
		Writer.WriteNil();
		FMsgPackReader Reader = MakeReader(Writer);
		bool bValue = false;
		TestTrue(TEXT("read true"), Reader.ReadBool(bValue));
		TestTrue(TEXT("true decodes as true"), bValue);
		TestTrue(TEXT("read false"), Reader.ReadBool(bValue));
		TestFalse(TEXT("false decodes as false"), bValue);
		TestTrue(TEXT("nil is consumed"), Reader.TryReadNil());
		TestTrue(TEXT("all consumed"), Reader.IsAtEnd());
	}

	// Container headers at their own width boundaries.
	const TArray<int32> ContainerCases = {0, 1, 15, 16, 65535, 65536};
	for (int32 Count : ContainerCases)
	{
		FMsgPackWriter MapWriter;
		MapWriter.WriteMapHeader(Count);
		FMsgPackReader MapReader = MakeReader(MapWriter);
		int32 DecodedCount = -1;
		TestTrue(TEXT("read map header"), MapReader.ReadMapHeader(DecodedCount));
		TestEqual(FString::Printf(TEXT("map header %d round-trips"), Count), DecodedCount, Count);

		FMsgPackWriter ArrayWriter;
		ArrayWriter.WriteArrayHeader(Count);
		FMsgPackReader ArrayReader = MakeReader(ArrayWriter);
		DecodedCount = -1;
		TestTrue(TEXT("read array header"), ArrayReader.ReadArrayHeader(DecodedCount));
		TestEqual(FString::Printf(TEXT("array header %d round-trips"), Count), DecodedCount, Count);
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexMsgPackSkipValueTest,
	"ApexSim.Net.MsgPack.SkipValue",
	ApexTestFlags)

bool FApexMsgPackSkipValueTest::RunTest(const FString& Parameters)
{
	// A nested value followed by a sentinel: if SkipValue lands anywhere but
	// exactly the end of the value, the sentinel will not decode.
	FMsgPackWriter Writer;
	Writer.WriteMapHeader(2);
	Writer.WriteString("nested");
	{
		Writer.WriteArrayHeader(3);
		for (int32 i = 0; i < 3; ++i)
		{
			Writer.WriteMapHeader(2);
			Writer.WriteString("x");
			Writer.WriteFloat(1.5f * i);
			Writer.WriteString("y");
			Writer.WriteFloat(-2.25f * i);
		}
	}
	Writer.WriteString("sentinel");
	Writer.WriteUInt(4242);

	FMsgPackReader Reader = MakeReader(Writer);

	int32 PairCount = 0;
	TestTrue(TEXT("read outer map"), Reader.ReadMapHeader(PairCount));
	TestEqual(TEXT("outer map has 2 pairs"), PairCount, 2);

	FString Key;
	TestTrue(TEXT("read first key"), Reader.ReadString(Key));
	TestEqual(TEXT("first key"), Key, FString(TEXT("nested")));
	TestTrue(TEXT("skip the nested array"), Reader.SkipValue());

	TestTrue(TEXT("read sentinel key"), Reader.ReadString(Key));
	TestEqual(TEXT("sentinel key survived the skip"), Key, FString(TEXT("sentinel")));
	uint64 Sentinel = 0;
	TestTrue(TEXT("read sentinel value"), Reader.ReadUInt64(Sentinel));
	TestEqual(TEXT("sentinel value survived the skip"), Sentinel, static_cast<uint64>(4242));
	TestTrue(TEXT("nothing left over"), Reader.IsAtEnd());

	// Skipping each primitive in turn must leave the cursor exactly at the end.
	{
		FMsgPackWriter Primitives;
		Primitives.WriteNil();
		Primitives.WriteBool(true);
		Primitives.WriteUInt(300);
		Primitives.WriteInt(-70000);
		Primitives.WriteFloat(1.0f);
		Primitives.WriteString(FString::ChrN(300, TEXT('q')));
		Primitives.WriteArrayHeader(0);
		Primitives.WriteMapHeader(0);

		FMsgPackReader PrimitiveReader = MakeReader(Primitives);
		for (int32 i = 0; i < 8; ++i)
		{
			TestTrue(FString::Printf(TEXT("skip primitive %d"), i), PrimitiveReader.SkipValue());
		}
		TestTrue(TEXT("skipped every primitive exactly"), PrimitiveReader.IsAtEnd());
	}

	// The depth cap must reject rather than blow the stack.
	{
		FMsgPackWriter Deep;
		for (int32 i = 0; i < 64; ++i)
		{
			Deep.WriteArrayHeader(1);
		}
		Deep.WriteNil();
		FMsgPackReader DeepReader = MakeReader(Deep);
		TestFalse(TEXT("excessive nesting is rejected"), DeepReader.SkipValue());
		TestTrue(TEXT("excessive nesting latches an error"), DeepReader.HasError());
	}

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexMsgPackTruncatedTest,
	"ApexSim.Net.MsgPack.Truncated",
	ApexTestFlags)

bool FApexMsgPackTruncatedTest::RunTest(const FString& Parameters)
{
	// Build a realistic nested value, then feed the reader every prefix of it.
	// None may read out of bounds, and none but the full buffer may succeed.
	FMsgPackWriter Writer;
	Writer.WriteMapHeader(3);
	Writer.WriteString("Id");
	Writer.WriteString(TEXT("11111111-2222-3333-4444-555555555555"));
	Writer.WriteString("Name");
	Writer.WriteString(TEXT("Nürburgring"));
	Writer.WriteString("Points");
	Writer.WriteArrayHeader(2);
	Writer.WriteMapHeader(2);
	Writer.WriteString("x");
	Writer.WriteFloat(1.5f);
	Writer.WriteString("y");
	Writer.WriteFloat(-2.25f);
	Writer.WriteMapHeader(2);
	Writer.WriteString("x");
	Writer.WriteFloat(3.0f);
	Writer.WriteString("y");
	Writer.WriteFloat(4.0f);

	const TArray<uint8>& Full = Writer.GetBuffer();

	for (int32 PrefixLength = 0; PrefixLength < Full.Num(); ++PrefixLength)
	{
		FMsgPackReader Reader(TArrayView<const uint8>(Full.GetData(), PrefixLength));
		const bool bSkipped = Reader.SkipValue();
		TestFalse(
			FString::Printf(TEXT("prefix of %d bytes must not decode as a whole value"), PrefixLength),
			bSkipped);
		TestTrue(
			FString::Printf(TEXT("prefix of %d bytes latches an error"), PrefixLength),
			Reader.HasError());
		TestTrue(
			FString::Printf(TEXT("prefix of %d bytes never advances past its end"), PrefixLength),
			Reader.Tell() <= PrefixLength);
	}

	// Braces, not parentheses: `FMsgPackReader R(TArrayView<const uint8>(Full))`
	// is a function declaration, not a variable (most vexing parse).
	FMsgPackReader FullReader{TArrayView<const uint8>(Full)};
	TestTrue(TEXT("the complete buffer skips cleanly"), FullReader.SkipValue());
	TestTrue(TEXT("the complete buffer is fully consumed"), FullReader.IsAtEnd());

	// A declared length far beyond the buffer must fail, not over-read.
	{
		const uint8 LyingStr32[] = {MsgPack::Str32, 0x7F, 0xFF, 0xFF, 0xFF, 0x41};
		FMsgPackReader Reader(TArrayView<const uint8>(LyingStr32, UE_ARRAY_COUNT(LyingStr32)));
		FString Decoded;
		TestFalse(TEXT("an overlong declared string length is rejected"), Reader.ReadString(Decoded));
		TestTrue(TEXT("an overlong declared string length latches an error"), Reader.HasError());
	}

	// A type mismatch must fail rather than silently reinterpret.
	{
		FMsgPackWriter Mismatch;
		Mismatch.WriteString(TEXT("not a number"));
		FMsgPackReader Reader = MakeReader(Mismatch);
		uint64 Value = 0;
		TestFalse(TEXT("reading a string as an integer fails"), Reader.ReadUInt64(Value));
		TestTrue(TEXT("the type mismatch latches an error"), Reader.HasError());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
