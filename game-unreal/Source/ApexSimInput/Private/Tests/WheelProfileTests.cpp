#include "ApexWheelProfiles.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace ApexWheelProfiles;

namespace
{
	constexpr EAutomationTestFlags ApexWheelProfileTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexWheelProfilesDetectTest,
	"ApexSim.Input.WheelProfiles.Detect",
	ApexWheelProfileTestFlags)

bool FApexWheelProfilesDetectTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		uint16 VendorId;
		const TCHAR* Expected;
	};
	const FCase Cases[] = {
		{ TEXT("FANATEC ClubSport Wheel Base V2.5"),        0x0EB7, TEXT("fanatec-csw-v25") },
		{ TEXT("FANATEC ClubSport Wheel Base V2"),          0x0EB7, TEXT("fanatec-csw-v2") },
		{ TEXT("FANATEC ClubSport DD"),                     0x0EB7, TEXT("fanatec-csw-dd") },
		{ TEXT("FANATEC CSL DD"),                           0x0EB7, TEXT("fanatec-csl-dd") },
		{ TEXT("FANATEC Podium Wheel Base DD2"),            0x0EB7, TEXT("fanatec-dd2") },
		{ TEXT("FANATEC CSL Elite Wheel Base"),             0x0EB7, TEXT("fanatec-csl-elite") },
		// Pedals named after the base they go with are not that base.
		{ TEXT("FANATEC CSL Elite Pedals V2"),              0x0EB7, TEXT("generic") },
		{ TEXT("Thrustmaster T-LCM Pedals"),                0x044F, TEXT("generic") },
		{ TEXT("Logitech G29 Driving Force Racing Wheel"),  0x046D, TEXT("logitech-g29") },
		{ TEXT("Logitech G923 Racing Wheel for Xbox and PC"), 0x046D, TEXT("logitech-g923") },
		{ TEXT("PRO Racing Wheel"),                         0x046D, TEXT("logitech-pro") },
		{ TEXT("Thrustmaster T300RS Racing wheel"),         0x044F, TEXT("thrustmaster-t300") },
		{ TEXT("T300RS GT Edition"),                        0x044F, TEXT("thrustmaster-t300") },
		{ TEXT("Thrustmaster T-GT II"),                     0x044F, TEXT("thrustmaster-t-gt") },
		{ TEXT("Thrustmaster TMX Racing Wheel"),            0x044F, TEXT("thrustmaster-tmx") },
		{ TEXT("Thrustmaster TX Racing Wheel"),             0x044F, TEXT("thrustmaster-tx") },
		{ TEXT("Thrustmaster TS-PC Racer"),                 0x044F, TEXT("thrustmaster-ts-pc") },
		{ TEXT("Gudsen MOZA R9 Base"),                      0x346E, TEXT("moza-r9") },
		{ TEXT("MOZA R12 Base"),                            0x346E, TEXT("moza-r12") },
		{ TEXT("MOZA R21 Base"),                            0x346E, TEXT("moza-r21") },
		{ TEXT("Simucube 2 Pro"),                           0x16D0, TEXT("simucube-2-pro") },
		{ TEXT("Simucube 2 Ultimate"),                      0x16D0, TEXT("simucube-2-ultimate") },
		// The vendor id is shared, so the name has to say Simucube.
		{ TEXT("Pro Controller"),                           0x16D0, TEXT("generic") },
		// The right name from the wrong maker is not that base.
		{ TEXT("G29 lookalike"),                            0x1234, TEXT("generic") },
		{ TEXT(""),                                         0x0000, TEXT("generic") },
	};
	for (const FCase& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("\"%s\""), Case.Name), FString(Detect(Case.Name, Case.VendorId).Id), FString(Case.Expected));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexWheelProfilesTableTest,
	"ApexSim.Input.WheelProfiles.Table",
	ApexWheelProfileTestFlags)

bool FApexWheelProfilesTableTest::RunTest(const FString& Parameters)
{
	const TConstArrayView<FProfile> Profiles = All();
	TestEqual(TEXT("Generic comes first"), IndexOf(Generic()), 0);
	TestEqual(TEXT("Generic has no peak"), Generic().PeakTorqueNm, 0.0f);

	TSet<FString> Ids;
	for (const FProfile& Profile : Profiles)
	{
		const FString Id = Profile.Id;
		TestFalse(FString::Printf(TEXT("%s is listed once"), *Id), Ids.Contains(Id));
		Ids.Add(Id);
		TestTrue(FString::Printf(TEXT("%s is found by id"), *Id), Find(Id) == &Profile);
		TestTrue(FString::Printf(TEXT("%s's peak is one the Wheel page can give"), *Id),
			Profile.PeakTorqueNm == 0.0f
			|| (Profile.PeakTorqueNm >= MinPeakTorqueNm && Profile.PeakTorqueNm <= MaxPeakTorqueNm));
	}
	TestNull(TEXT("an unknown id"), Find(TEXT("no-such-base")));

	// The feel was tuned on the reference base: it, any weaker base and an
	// unknown one mix exactly as they always did.
	const FProfile* Reference = Find(TEXT("fanatec-csw-v25"));
	TestNotNull(TEXT("the reference base has a profile"), Reference);
	if (Reference)
	{
		TestEqual(TEXT("the reference base is the reference"), Reference->PeakTorqueNm, ReferencePeakTorqueNm);
		TestEqual(TEXT("and plays at full scale"), OutputScale(Reference->PeakTorqueNm), 1.0f);
		TestEqual(TEXT("with no minimum force"), MinimumForce(Reference->Drive), 0.0f);
	}
	TestEqual(TEXT("an unknown peak is not scaled"), OutputScale(0.0f), 1.0f);
	TestEqual(TEXT("nor a weak base"), OutputScale(2.2f), 1.0f);
	TestEqual(TEXT("nor a NaN"), OutputScale(NAN), 1.0f);
	TestTrue(TEXT("a 25 Nm base plays the reference's newton-metres"), FMath::IsNearlyEqual(OutputScale(25.0f), 8.0f / 25.0f));
	TestEqual(TEXT("Generic has no minimum force"), MinimumForce(Generic().Drive), 0.0f);
	TestTrue(TEXT("a gear drive has one"), MinimumForce(EDrive::Gear) > 0.0f);
	TestEqual(TEXT("a direct drive has none"), MinimumForce(EDrive::Direct), 0.0f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
