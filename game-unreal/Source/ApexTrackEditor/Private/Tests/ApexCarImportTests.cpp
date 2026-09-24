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

	TestFalse(TEXT("no [wheels] table"), Car.Wheels.IsPresent());
	TestTrue(TEXT("no wheels, no spec"), !UApexCarImportCommandlet::MakeWheelSpec(Car, nullptr).IsUsable());

	UApexCarImportCommandlet::FCarToml Bare;
	TestFalse(TEXT("no id is an error"), UApexCarImportCommandlet::ParseCarToml(TEXT("name = \"x\""), Bare, Error));
	TestEqual(TEXT("folder to segment"), UApexCarImportCommandlet::PackageSegment(TEXT("yotota-lmp2")),
		FString(TEXT("yotota_lmp2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlWheelsTest, "ApexSim.Cars.TomlWheels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApexCarTomlWheelsTest::RunTest(const FString& Parameters)
{
	const FString Text = TEXT(
		"id = \"a1b2\"\n"
		"name = \"F1\"\n"
		"[physics]\n"
		"max_steering_angle_rad = 0.35\n"
		"wheelbase_m = 3.6\n"
		"[[engine.torque_curve]]\n"
		"rpm = 5000.0\n"
		"# a comment\n"
		"[wheels]\n"
		"model = \"f1\"   # content/wheels/f1.glb\n"
		"front_axle_m = 1.560\n"
		"rear_axle_m = -1.985\n"
		"front_track_m = 1.627\n"
		"rear_track_m = 1.501\n"
		"front_radius_m = 0.328\n"
		"rear_radius_m = 0.316\n"
		"front_width_m = 0.333\n"
		"rear_width_m = 0.410\n");
	UApexCarImportCommandlet::FCarToml Car;
	FString Error;
	TestTrue(TEXT("parses"), UApexCarImportCommandlet::ParseCarToml(Text, Car, Error));
	TestTrue(TEXT("has wheels"), Car.Wheels.IsPresent());
	TestEqual(TEXT("model"), Car.Wheels.Model, FString(TEXT("f1")));
	TestEqual(TEXT("front axle"), Car.Wheels.FrontAxleM, 1.56f);
	TestEqual(TEXT("rear axle is behind"), Car.Wheels.RearAxleM, -1.985f);
	TestEqual(TEXT("rear width"), Car.Wheels.RearWidthM, 0.41f);
	TestEqual(TEXT("steering lock from [physics]"), Car.MaxSteerRad, 0.35f);

	const FApexWheelSpec Spec = UApexCarImportCommandlet::MakeWheelSpec(
		Car, TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Cars/Wheels/f1/SM_Wheel_f1.SM_Wheel_f1"))));
	TestTrue(TEXT("spec is usable"), Spec.IsUsable());
	TestEqual(TEXT("spec carries the lock"), Spec.MaxSteerRad, 0.35f);
	TestEqual(TEXT("spec rear radius"), Spec.RearRadiusM, 0.316f);
	TestEqual(TEXT("wheel package"), UApexCarImportCommandlet::WheelPackageName(TEXT("/Game/Cars"), TEXT("f1")),
		FString(TEXT("/Game/Cars/Wheels/f1/SM_Wheel_f1")));

	// Axles the wrong way round, or a zero radius, would draw nonsense.
	UApexCarImportCommandlet::FCarToml Swapped;
	TestFalse(TEXT("rear ahead of front is an error"), UApexCarImportCommandlet::ParseCarToml(
		Text.Replace(TEXT("front_axle_m = 1.560"), TEXT("front_axle_m = -3.0")), Swapped, Error));
	UApexCarImportCommandlet::FCarToml Flat;
	TestFalse(TEXT("zero radius is an error"), UApexCarImportCommandlet::ParseCarToml(
		Text.Replace(TEXT("front_radius_m = 0.328"), TEXT("front_radius_m = 0")), Flat, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlSoundTest, "ApexSim.Cars.TomlSound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApexCarTomlSoundTest::RunTest(const FString& Parameters)
{
	const FString Text = TEXT(
		"id = \"a1b2\"\n"
		"name = \"V8\"\n"
		"[engine]\n"
		"idle_rpm = 850.0\n"
		"redline_rpm = 7500.0\n"
		"rev_limiter_rpm = 7650.0\n"
		"[[engine.torque_curve]]\n"
		"rpm = 833.3   # not the idle\n"
		"[sound]\n"
		"cylinders = 8\n"
		"crossplane = true\n"
		"turbo = false\n"
		"exhaust_length_m = 1.9\n"
		"muffling = 0.45\n"
		"pops = 0.9\n"
		"gear_whine = 0.3\n"
		"intake_roar = 0.6\n");
	UApexCarImportCommandlet::FCarToml Car;
	FString Error;
	TestTrue(TEXT("parses"), UApexCarImportCommandlet::ParseCarToml(Text, Car, Error));
	TestEqual(TEXT("cylinders"), Car.Sound.Cylinders, 8);
	TestTrue(TEXT("crossplane"), Car.Sound.bCrossplane);
	TestFalse(TEXT("no turbo"), Car.Sound.bTurbo);
	TestEqual(TEXT("idle from [engine]"), Car.Sound.IdleRpm, 850.0f);
	TestEqual(TEXT("redline from [engine]"), Car.Sound.RedlineRpm, 7500.0f);
	TestEqual(TEXT("limiter from [engine]"), Car.Sound.LimiterRpm, 7650.0f);
	TestEqual(TEXT("exhaust"), Car.Sound.ExhaustLengthM, 1.9f);
	TestEqual(TEXT("pops"), Car.Sound.Pops, 0.9f);

	// A car without the table still carries its rev range: the class picks the rest.
	UApexCarImportCommandlet::FCarToml Plain;
	TestTrue(TEXT("parses without [sound]"), UApexCarImportCommandlet::ParseCarToml(
		TEXT("id = \"a\"\nname = \"b\"\n[engine]\nidle_rpm = 900\nredline_rpm = 9000\n"), Plain, Error));
	TestEqual(TEXT("no cylinders means no table"), Plain.Sound.Cylinders, 0);
	TestEqual(TEXT("redline kept"), Plain.Sound.RedlineRpm, 9000.0f);

	UApexCarImportCommandlet::FCarToml Bad;
	TestFalse(TEXT("a share past 1 is an error"), UApexCarImportCommandlet::ParseCarToml(
		TEXT("id = \"a\"\nname = \"b\"\n[sound]\ncylinders = 6\nexhaust_length_m = 1.0\npops = 3\n"), Bad, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarTomlLiveryTest, "ApexSim.Cars.TomlLivery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApexCarTomlLiveryTest::RunTest(const FString& Parameters)
{
	const FString Text = TEXT(
		"id = \"a1b2\"\n"
		"name = \"Hypercar\"\n"
		"[[livery]]\n"
		"name = \"Blu\"   # a comment\n"
		"paint = [0.02, 0.10, 0.45]\n"
		"accent = [0.9, 0.9, 0.9]\n"
		"metallic = 0.6\n"
		"logo = \"textures/blu_logo.png\"\n"
		"[[livery]]\n"
		"name = \"Plain\"\n"
		"paint = [1, 1, 1]\n"
		"[wheels]\n"
		"model = \"gt3\"\n"
		"front_axle_m = 1.4\n"
		"rear_axle_m = -1.4\n"
		"front_track_m = 1.6\n"
		"rear_track_m = 1.6\n"
		"front_radius_m = 0.35\n"
		"rear_radius_m = 0.35\n"
		"front_width_m = 0.3\n"
		"rear_width_m = 0.3\n");
	UApexCarImportCommandlet::FCarToml Car;
	FString Error;
	TestTrue(TEXT("parses"), UApexCarImportCommandlet::ParseCarToml(Text, Car, Error));
	if (!TestEqual(TEXT("two liveries"), Car.Liveries.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("name"), Car.Liveries[0].Name, FString(TEXT("Blu")));
	TestEqual(TEXT("paint"), Car.Liveries[0].Paint, FLinearColor(0.02f, 0.10f, 0.45f, 1.0f));
	TestEqual(TEXT("accent"), Car.Liveries[0].Accent, FLinearColor(0.9f, 0.9f, 0.9f, 1.0f));
	TestEqual(TEXT("metallic"), Car.Liveries[0].Metallic, 0.6f);
	TestEqual(TEXT("logo"), Car.Liveries[0].Logo, FString(TEXT("textures/blu_logo.png")));
	TestTrue(TEXT("no accent keeps the model's"), Car.Liveries[1].Accent.A == 0.0f);
	TestEqual(TEXT("no metallic keeps the model's"), Car.Liveries[1].Metallic, -1.0f);
	TestTrue(TEXT("a later table is not a livery's"), Car.Wheels.IsPresent());
	TestEqual(TEXT("logo package"),
		UApexCarImportCommandlet::LiveryLogoPackageName(TEXT("/Game/Cars"), TEXT("bugotti-chiffon-hypercar"), TEXT("textures/blu-logo.png")),
		FString(TEXT("/Game/Cars/bugotti_chiffon_hypercar/Liveries/T_blu_logo")));

	UApexCarImportCommandlet::FCarToml NoPaint;
	TestFalse(TEXT("a livery without paint is an error"), UApexCarImportCommandlet::ParseCarToml(
		TEXT("id = \"a\"\nname = \"b\"\n[[livery]]\nname = \"c\"\n"), NoPaint, Error));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
