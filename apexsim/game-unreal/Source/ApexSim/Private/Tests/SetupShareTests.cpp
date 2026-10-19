#include "ApexSetupShare.h"
#include "ApexTestCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSetupShareRoundTripTest,
	"ApexSim.SetupShare.RoundTrip",
	ApexTestFlags)

bool FApexSetupShareRoundTripTest::RunTest(const FString& Parameters)
{
	ApexSetupShare::FShared Shared;
	Shared.CarId = TEXT("fugazzi-sf26");
	Shared.CarCrc = 0x1A2B3C4Du;
	Shared.Name = TEXT("Monza; low=drag\nrun");
	Shared.Setup.Clicks[0] = 3;
	Shared.Setup.Clicks[4] = -2;

	const FString Line = ApexSetupShare::Encode(Shared);
	TestTrue(TEXT("starts with the version tag"), Line.StartsWith(TEXT("APXS1;car=fugazzi-sf26;crc=1A2B3C4D;")));
	TestFalse(TEXT("one line"), Line.Contains(TEXT("\n")));

	// As a chat would carry it: inside backticks, after some words.
	ApexSetupShare::FShared Read;
	FString Error;
	TestTrue(TEXT("decodes from a chat message"),
		ApexSetupShare::Decode(FString::Printf(TEXT("try this one: `%s` good luck"), *Line), Read, Error));
	TestEqual(TEXT("car"), Read.CarId, Shared.CarId);
	TestEqual(TEXT("crc"), Read.CarCrc, Shared.CarCrc);
	TestEqual(TEXT("the name, without the separators"), Read.Name, FString(TEXT("Monza  low drag run")));
	TestEqual(TEXT("knob 0"), Read.Setup.GetClick(0), 3);
	TestEqual(TEXT("knob 4"), Read.Setup.GetClick(4), -2);
	TestEqual(TEXT("every knob"), Read.Setup.Clicks.Num(), FApexCarSetup::KnobCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexSetupShareRejectsTest,
	"ApexSim.SetupShare.Rejects",
	ApexTestFlags)

bool FApexSetupShareRejectsTest::RunTest(const FString& Parameters)
{
	ApexSetupShare::FShared Read;
	FString Error;
	TestFalse(TEXT("plain text is no setup"), ApexSetupShare::Decode(TEXT("hello"), Read, Error));
	TestFalse(TEXT("the reason says so"), Error.IsEmpty());
	TestFalse(TEXT("no car"), ApexSetupShare::Decode(TEXT("APXS1;name=x;c=1,2"), Read, Error));
	TestFalse(TEXT("no clicks"), ApexSetupShare::Decode(TEXT("APXS1;car=x;name=y"), Read, Error));
	TestFalse(TEXT("damaged clicks"), ApexSetupShare::Decode(TEXT("APXS1;car=x;c=1,zz,3"), Read, Error));

	// A short list (an older version with fewer knobs) fills the rest at stock,
	// an unknown key is skipped, and a click out of range is pinned.
	TestTrue(TEXT("short list"), ApexSetupShare::Decode(TEXT("APXS1;car=x;future=1;c=1,999"), Read, Error));
	TestEqual(TEXT("knob 0 kept"), Read.Setup.GetClick(0), 1);
	TestTrue(TEXT("knob 1 pinned"), Read.Setup.GetClick(1) < 999);
	TestEqual(TEXT("the rest at stock"), Read.Setup.GetClick(10), 0);
	TestEqual(TEXT("an unnamed setup gets a name"), Read.Name, FString(TEXT("Pasted setup")));
	TestEqual(TEXT("no crc reads 0"), Read.CarCrc, 0u);

	ApexSetupShare::FShared Named;
	Named.CarId = TEXT("posh-gt3rs");
	Named.Name = TEXT("Spa / wet: v2");
	TestEqual(TEXT("a file name safe anywhere"), ApexSetupShare::FileName(Named), FString(TEXT("posh-gt3rs-Spa___wet__v2.txt")));
	return true;
}

#endif
