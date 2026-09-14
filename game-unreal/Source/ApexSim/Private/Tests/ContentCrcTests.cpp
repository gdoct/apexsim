#include "ApexTestCommon.h"
#include "Catalog/ApexContentCrc.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	TArray<uint8> Bytes(const ANSICHAR* Text)
	{
		TArray<uint8> Out;
		Out.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		return Out;
	}
}

// The checksum is a contract with content_crc.rs and build_track_catalog.py:
// all three must produce the standard CRC-32, or a matching file reads as a
// mismatch on every start.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexContentCrcVectorTest,
	"ApexSim.Content.CrcVector",
	ApexTestFlags)

bool FApexContentCrcVectorTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("standard check vector"), ApexContentCrc::Compute(Bytes("123456789")), 0xCBF43926u);
	TestEqual(TEXT("empty input"), ApexContentCrc::Compute(TArrayView<const uint8>()), 0u);
	TestNotEqual(TEXT("one byte changes it"),
		ApexContentCrc::Compute(Bytes("width_m: 12")), ApexContentCrc::Compute(Bytes("width_m: 13")));
	return true;
}

// A Windows checkout with autocrlf must hash like the Linux server's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexContentCrcLineEndingsTest,
	"ApexSim.Content.CrcLineEndings",
	ApexTestFlags)

bool FApexContentCrcLineEndingsTest::RunTest(const FString& Parameters)
{
	const uint32 Lf = ApexContentCrc::Compute(Bytes("name: Monza\ntrack_id: abc\n"));
	const uint32 Crlf = ApexContentCrc::Compute(Bytes("name: Monza\r\ntrack_id: abc\r\n"));
	TestEqual(TEXT("CRLF hashes like LF"), Crlf, Lf);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexContentCompareTest,
	"ApexSim.Content.Compare",
	ApexTestFlags)

bool FApexContentCompareTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("equal is a match"), ApexContent::Compare(0xCBF43926, 0xCBF43926) == EApexContentMatch::Match);
	TestTrue(TEXT("different is a mismatch"), ApexContent::Compare(0xCBF43926, 0xDEADBEEF) == EApexContentMatch::Mismatch);
	// An old server, or a row synced before the field existed, says nothing
	// either way and must not raise a warning on every race.
	TestTrue(TEXT("no server checksum is unknown"), ApexContent::Compare(0xCBF43926, 0) == EApexContentMatch::Unknown);
	TestTrue(TEXT("no local checksum is unknown"), ApexContent::Compare(0, 0xCBF43926) == EApexContentMatch::Unknown);
	TestTrue(TEXT("neither is unknown"), ApexContent::Compare(0, 0) == EApexContentMatch::Unknown);

	const FString Message = ApexContent::DescribeMismatch(TEXT("Track"), TEXT("Monza"), 0xCBF43926, 0xDEADBEEF);
	TestTrue(TEXT("message names the content"), Message.Contains(TEXT("Track \"Monza\"")));
	TestTrue(TEXT("message carries both checksums"), Message.Contains(TEXT("CBF43926")) && Message.Contains(TEXT("DEADBEEF")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
