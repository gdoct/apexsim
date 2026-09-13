#include "ApexCarImportCommandlet.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlTest, "ApexSim.Cars.Toml",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApexCarTomlTest::RunTest(const FString& Parameters)
{
	const FString Text = TEXT(
		"id = \"f7f70d97-08b6-4787-ac47-2f096a75f371\"\n"
		"name = \"Yotota LMP2\"   # the display name\n"
		"version = \"1.0.0\"\n"
		"model = \"yotota_lmp2.glb\"\n"
		"brand = \"Yotota\"\n"
		"model_year = 2026\n"
		"class = \"LMP2\"\n"
		"manufacturer_country = \"Japan\"\n"
		"\n"
		"[physics]\n"
		"mass_kg = 930.0\n"
		"# a comment with mass_kg = 1 in it\n"
		"[engine]\n"
		"max_power_w = 405000.0\n"
		"[[engine.torque_curve]]\n"
		"rpm = 1000\n");
	UApexCarImportCommandlet::FCarToml Car;
	FString Error;
	TestTrue(TEXT("parses"), UApexCarImportCommandlet::ParseCarToml(Text, Car, Error));
	TestEqual(TEXT("id"), Car.Id, FString(TEXT("f7f70d97-08b6-4787-ac47-2f096a75f371")));
	TestEqual(TEXT("name without the comment"), Car.Name, FString(TEXT("Yotota LMP2")));
	TestEqual(TEXT("model"), Car.Model, FString(TEXT("yotota_lmp2.glb")));
	TestEqual(TEXT("class"), Car.CarClass, FString(TEXT("LMP2")));
	TestEqual(TEXT("country"), Car.ManufacturerCountry, FString(TEXT("Japan")));
	TestEqual(TEXT("year"), Car.ModelYear, 2026);
	TestEqual(TEXT("mass"), Car.MassKg, 930.0f);
	TestEqual(TEXT("power in kW"), Car.MaxPowerKw, 405.0f);

	UApexCarImportCommandlet::FCarToml Bare;
	TestFalse(TEXT("no id is an error"), UApexCarImportCommandlet::ParseCarToml(TEXT("name = \"x\""), Bare, Error));
	TestEqual(TEXT("folder to segment"), UApexCarImportCommandlet::PackageSegment(TEXT("yotota-lmp2")),
		FString(TEXT("yotota_lmp2")));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
