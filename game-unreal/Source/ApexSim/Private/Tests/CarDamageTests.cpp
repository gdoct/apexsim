#include "ApexTestCommon.h"
#include "Cars/ApexCarToml.h"
#include "Cars/ApexGlbReader.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Race/ApexCarDamage.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const TCHAR* DamageTomlHead = TEXT("id = \"d\"\nname = \"Damage test\"\nmodel = \"car.glb\"\n\n");

	/** A one-material model of triangles centred at `Centres` (cm), each a small tent around its centre. */
	FApexGlbModel DamageModel(std::initializer_list<FVector3f> Centres)
	{
		FApexGlbModel Model;
		Model.Materials.AddDefaulted_GetRef().Name = TEXT("car_paint");
		Model.Materials.AddDefaulted_GetRef().Name = TEXT("car_carbon");
		Model.Sections.SetNum(2);
		Model.Sections[0].Material = 0;
		Model.Sections[1].Material = 1;
		int32 n = 0;
		for (const FVector3f& Centre : Centres)
		{
			// Every other triangle in the carbon section, so a piece has to keep both.
			FApexGlbSection& Section = Model.Sections[n++ % 2];
			for (const FVector3f& Corner : {FVector3f(-1.0f, -1.0f, 0.0f), FVector3f(2.0f, -1.0f, 0.0f), FVector3f(-1.0f, 2.0f, 0.0f)})
			{
				Section.Indices.Add(Model.Positions.Add(Centre + Corner));
				Model.Normals.Add(FVector3f::UpVector);
				Model.UVs.Add(FVector2f::ZeroVector);
				Model.Bounds += Centre + Corner;
			}
		}
		return Model;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarDamageTomlTest, "ApexSim.Cars.Damage.Toml", ApexTestFlags)

bool FApexCarDamageTomlTest::RunTest(const FString& Parameters)
{
	const FString Text = FString(DamageTomlHead) + TEXT(
		"[[damage_part]]\n"
		"name = \"front_wing\"\n"
		"zone = \"front\"\n"
		"detach_pct = 30   # comes off at 30%\n"
		"min_m = [2.4, -1.0, -0.1]\n"
		"max_m = [3.2, 1.0, 0.45]\n"
		"\n"
		"[[damage_part]]\n"
		"name = \"rear_wing\"\n"
		"zone = \"Rear\"\n"
		"detach_pct = 45\n"
		"min_m = [-2.7, -0.8, 0.5]\n"
		"max_m = [-1.9, 0.8, 1.1]\n");
	FApexCarToml Toml;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("parses (%s)"), *Error), ApexCarToml::Parse(Text, Toml, Error)))
	{
		return false;
	}
	const TArray<FApexDamagePartSpec> Parts = ApexCarToml::MakeDamageParts(Toml);
	if (!TestEqual(TEXT("two parts"), Parts.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("name"), Parts[0].Name, FString(TEXT("front_wing")));
	TestTrue(TEXT("front zone"), Parts[0].Zone == EApexDamageZone::Front);
	TestTrue(TEXT("zone names ignore case"), Parts[1].Zone == EApexDamageZone::Rear);
	TestEqual(TEXT("detach"), Parts[0].DetachPct, 30.0f);
	// The car's (forward, left, up) metres onto the mesh's (left, forward, up) centimetres.
	TestTrue(TEXT("min in the mesh frame"), Parts[0].MinCm.Equals(FVector(-100.0, 240.0, -10.0), 0.01));
	TestTrue(TEXT("max in the mesh frame"), Parts[0].MaxCm.Equals(FVector(100.0, 320.0, 45.0), 0.01));
	TestTrue(TEXT("a rear box behind the origin"), Parts[1].MaxCm.Y < 0.0 && Parts[1].MinCm.Y < Parts[1].MaxCm.Y);

	auto Rejects = [this](const TCHAR* What, const TCHAR* Body) {
		FApexCarToml Bad;
		FString Why;
		TestFalse(What, ApexCarToml::Parse(FString(DamageTomlHead) + Body, Bad, Why));
	};
	Rejects(TEXT("an unknown zone"), TEXT("[[damage_part]]\nname = \"x\"\nzone = \"roof\"\ndetach_pct = 50\nmin_m = [0, 0, 0]\nmax_m = [1, 1, 1]\n"));
	Rejects(TEXT("an inside-out box"), TEXT("[[damage_part]]\nname = \"x\"\nzone = \"front\"\ndetach_pct = 50\nmin_m = [1, 0, 0]\nmax_m = [0, 1, 1]\n"));
	Rejects(TEXT("no name"), TEXT("[[damage_part]]\nzone = \"front\"\ndetach_pct = 50\nmin_m = [0, 0, 0]\nmax_m = [1, 1, 1]\n"));
	Rejects(TEXT("no threshold"), TEXT("[[damage_part]]\nname = \"x\"\nzone = \"front\"\nmin_m = [0, 0, 0]\nmax_m = [1, 1, 1]\n"));
	Rejects(TEXT("a two-number box"), TEXT("[[damage_part]]\nname = \"x\"\nzone = \"front\"\ndetach_pct = 50\nmin_m = [0, 0]\nmax_m = [1, 1, 1]\n"));

	// A car without the tables has no parts, and a livery after the parts is still a livery.
	FApexCarToml Plain;
	TestTrue(TEXT("no tables parses"), ApexCarToml::Parse(FString(DamageTomlHead), Plain, Error));
	TestEqual(TEXT("no tables, no parts"), ApexCarToml::MakeDamageParts(Plain).Num(), 0);
	FApexCarToml Mixed;
	TestTrue(TEXT("parts then a livery parse"), ApexCarToml::Parse(Text + TEXT("\n[[livery]]\nname = \"L\"\npaint = [1, 0, 0]\n"), Mixed, Error));
	TestEqual(TEXT("the livery is read"), Mixed.Liveries.Num(), 1);
	TestEqual(TEXT("the parts are kept"), Mixed.DamageParts.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarDamageSplitTest, "ApexSim.Cars.Damage.Split", ApexTestFlags)

bool FApexCarDamageSplitTest::RunTest(const FString& Parameters)
{
	// Two triangles at the nose, one in the middle, one in both boxes' overlap.
	const FApexGlbModel Model = DamageModel({FVector3f(0.0f, 300.0f, 20.0f), FVector3f(50.0f, 290.0f, 10.0f),
		FVector3f(0.0f, 0.0f, 50.0f), FVector3f(0.0f, -250.0f, 100.0f)});
	const FBox3f Boxes[] = {
		FBox3f(FVector3f(-100.0f, 240.0f, -10.0f), FVector3f(100.0f, 320.0f, 45.0f)),
		FBox3f(FVector3f(-100.0f, -300.0f, 50.0f), FVector3f(100.0f, -200.0f, 150.0f)),
		// Holds the first rear triangle too: the first box to hold it wins.
		FBox3f(FVector3f(-100.0f, -300.0f, 0.0f), FVector3f(100.0f, -200.0f, 150.0f)),
	};
	TArray<FApexGlbModel> Pieces;
	ApexGlb::SplitByBoxes(Model, Boxes, Pieces);
	if (!TestEqual(TEXT("the body and three pieces"), Pieces.Num(), 4))
	{
		return false;
	}
	TestEqual(TEXT("the body keeps the middle triangle"), Pieces[0].NumTriangles(), 1);
	TestEqual(TEXT("the nose box takes both nose triangles"), Pieces[1].NumTriangles(), 2);
	TestEqual(TEXT("the tail box takes the tail"), Pieces[2].NumTriangles(), 1);
	TestEqual(TEXT("an overlapping later box gets nothing"), Pieces[3].NumTriangles(), 0);
	TestEqual(TEXT("an empty piece has no section"), Pieces[3].Sections.Num(), 0);
	TestEqual(TEXT("a piece keeps only its own vertices"), Pieces[1].Positions.Num(), 6);
	TestEqual(TEXT("... with a normal and a UV each"), Pieces[1].Normals.Num() + Pieces[1].UVs.Num(), 12);
	TestEqual(TEXT("the nose keeps both materials' sections"), Pieces[1].Sections.Num(), 2);
	TestEqual(TEXT("the materials are copied whole"), Pieces[1].Materials.Num(), 2);
	TestTrue(TEXT("the piece's bounds are its own"), Pieces[1].Bounds.Min.Y > 200.0f);
	int32 Total = 0;
	for (const FApexGlbModel& Piece : Pieces)
	{
		Total += Piece.NumTriangles();
		for (const FApexGlbSection& Section : Piece.Sections)
		{
			for (const uint32 Index : Section.Indices)
			{
				TestTrue(TEXT("every index is inside its piece"), Index < static_cast<uint32>(Piece.Positions.Num()));
			}
		}
	}
	TestEqual(TEXT("no triangle lost or doubled"), Total, Model.NumTriangles());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarDamageSharesTest, "ApexSim.Cars.Damage.Shares", ApexTestFlags)

bool FApexCarDamageSharesTest::RunTest(const FString& Parameters)
{
	const float Pct[ApexDamage::NumZones] = {-1.0f, 50.0f, 140.0f, 0.0f, 100.0f};
	const ApexDamage::FShares Shares = ApexDamage::FromPercent(Pct);
	TestEqual(TEXT("an unknown zone is undamaged"), Shares.Zone[ApexDamage::Front], 0.0f);
	TestEqual(TEXT("half"), Shares.Zone[ApexDamage::Rear], 0.5f);
	TestEqual(TEXT("clamped to whole"), Shares.Zone[ApexDamage::Left], 1.0f);
	TestTrue(TEXT("not zero"), !Shares.IsZero());
	TestTrue(TEXT("an old server's frame is no damage"), ApexDamage::FromPercent(TArray<float>({-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}).GetData()).IsZero());

	TestEqual(TEXT("no damage, no dent"), ApexDamage::Visual(0.0f), 0.0f);
	TestEqual(TEXT("a write-off, the whole dent"), ApexDamage::Visual(1.0f), 1.0f);
	TestTrue(TEXT("a 4% tap already shows"), ApexDamage::Visual(0.04f) > 0.12f);
	TestTrue(TEXT("more damage, more dent"), ApexDamage::Visual(0.3f) < ApexDamage::Visual(0.6f));

	FApexDamagePartSpec Wing;
	Wing.Zone = EApexDamageZone::Rear;
	Wing.DetachPct = 50.0f;
	TestTrue(TEXT("on at 49.9%"), ApexDamage::IsAttached(Wing, ApexDamage::FromPercent(TArray<float>({0.0f, 49.9f, 0.0f, 0.0f, 0.0f}).GetData())));
	TestFalse(TEXT("off at 50%"), ApexDamage::IsAttached(Wing, Shares));
	TestTrue(TEXT("another zone's damage leaves it on"), ApexDamage::IsAttached(Wing, ApexDamage::FromPercent(TArray<float>({100.0f, 0.0f, 100.0f, 100.0f, 100.0f}).GetData())));
	TestEqual(TEXT("left zone index"), ApexDamage::ZoneIndex(EApexDamageZone::Left), static_cast<int32>(ApexDamage::Left));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarDamageEmittersTest, "ApexSim.Cars.Damage.Emitters", ApexTestFlags)

bool FApexCarDamageEmittersTest::RunTest(const FString& Parameters)
{
	auto Engine = [](float Pct) { return ApexDamage::FromPercent(TArray<float>({0.0f, 0.0f, 0.0f, 0.0f, Pct}).GetData()); };
	TestEqual(TEXT("a healthy engine does not smoke"), ApexDamage::EngineSmokeRate(Engine(30.0f)), 0.0f);
	TestTrue(TEXT("a hurt one does"), ApexDamage::EngineSmokeRate(Engine(50.0f)) > 0.0f);
	TestTrue(TEXT("a blown one most"), ApexDamage::EngineSmokeRate(Engine(100.0f)) > ApexDamage::EngineSmokeRate(Engine(90.0f)));
	TestTrue(TEXT("the smoke darkens as it goes"), ApexDamage::EngineSmokeShade(Engine(95.0f)) < ApexDamage::EngineSmokeShade(Engine(40.0f)));

	const ApexDamage::FShares Nose = ApexDamage::FromPercent(TArray<float>({40.0f, 0.0f, 0.0f, 0.0f, 0.0f}).GetData());
	TestEqual(TEXT("a cool radiator does not steam"), ApexDamage::SteamRate(Nose, 95.0f), 0.0f);
	TestTrue(TEXT("a holed one running hot does"), ApexDamage::SteamRate(Nose, 112.0f) > 0.0f);
	TestEqual(TEXT("a hot engine with a sound nose does not"), ApexDamage::SteamRate(ApexDamage::FShares(), 115.0f), 0.0f);
	TestEqual(TEXT("no sparks at a crawl"), ApexDamage::ScrapeSparkRate(1.0f), 0.0f);
	TestTrue(TEXT("more sparks faster"), ApexDamage::ScrapeSparkRate(40.0f) > ApexDamage::ScrapeSparkRate(10.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarDamageDebrisTest, "ApexSim.Cars.Damage.Debris", ApexTestFlags)

bool FApexCarDamageDebrisTest::RunTest(const FString& Parameters)
{
	// A wing thrown off at 50 m/s, a metre up, spinning: it lands, bounces
	// lower each time, slides and stops, and never goes through the ground.
	ApexDamage::FDebris Wing;
	Wing.Location = FVector(0.0, 0.0, 100.0);
	Wing.Velocity = FVector(5000.0, 300.0, 400.0);
	Wing.Spin = FVector(2.0, 6.0, 1.0);
	Wing.GroundZ = 0.0f;
	bool bLanded = false;
	double LastPeak = 1.0e9;
	double Peak = 0.0;
	bool bRising = true;
	int32 Bounces = 0;
	float Time = 0.0f;
	for (; Time < 30.0f && !Wing.bResting; Time += 1.0f / 60.0f)
	{
		const double Before = Wing.Location.Z;
		ApexDamage::StepDebris(Wing, 1.0f / 60.0f);
		TestTrue(TEXT("never under the ground"), Wing.Location.Z >= Wing.GroundZ - 0.01);
		TestTrue(TEXT("the rotation stays a rotation"), FMath::IsNearlyEqual(Wing.Rotation.Size(), 1.0, 1.0e-3));
		bLanded |= Wing.Location.Z <= Wing.GroundZ + 0.01;
		if (bRising && Wing.Location.Z < Before)
		{
			bRising = false;
			TestTrue(TEXT("each bounce lower than the last"), Peak < LastPeak);
			LastPeak = Peak;
			++Bounces;
		}
		if (!bRising && Wing.Location.Z > Before)
		{
			bRising = true;
			Peak = 0.0;
		}
		Peak = FMath::Max(Peak, Wing.Location.Z);
	}
	TestTrue(TEXT("it lands"), bLanded);
	TestTrue(TEXT("it bounces"), Bounces >= 2);
	TestTrue(TEXT("it comes to rest"), Wing.bResting);
	TestTrue(TEXT("within a quarter of a minute"), Time < 15.0f);
	TestTrue(TEXT("down the road, not back up it"), Wing.Location.X > 1000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarDamagePuffsTest, "ApexSim.Cars.Damage.Puffs", ApexTestFlags)

bool FApexCarDamagePuffsTest::RunTest(const FString& Parameters)
{
	ApexDamage::FPuff Smoke;
	Smoke.Life = 2.0f;
	Smoke.StartSize = 20.0f;
	Smoke.EndSize = 160.0f;
	Smoke.Opacity = 0.5f;
	Smoke.Velocity = FVector(3000.0, 0.0, 60.0);
	Smoke.Gravity = -0.04f;
	TestEqual(TEXT("born invisible"), ApexDamage::PuffOpacity(Smoke), 0.0f);
	float LastRadius = ApexDamage::PuffRadius(Smoke);
	float PeakOpacity = 0.0f;
	int32 Steps = 0;
	while (ApexDamage::StepPuff(Smoke, 0.05f))
	{
		++Steps;
		const float Radius = ApexDamage::PuffRadius(Smoke);
		TestTrue(TEXT("it only grows"), Radius >= LastRadius);
		LastRadius = Radius;
		PeakOpacity = FMath::Max(PeakOpacity, ApexDamage::PuffOpacity(Smoke));
		TestTrue(TEXT("never past its opacity"), ApexDamage::PuffOpacity(Smoke) <= Smoke.Opacity + 1.0e-4f);
	}
	TestTrue(TEXT("it lived its life"), Steps >= 38 && Steps <= 40);
	TestTrue(TEXT("it showed"), PeakOpacity > 0.4f);
	TestEqual(TEXT("gone at the end"), ApexDamage::PuffOpacity(Smoke), 0.0f);
	TestTrue(TEXT("it rose"), Smoke.Location.Z > 0.0);
	TestTrue(TEXT("the air slowed it"), Smoke.Velocity.X < 300.0);

	ApexDamage::FPuff Spark;
	Spark.bSpark = true;
	Spark.Life = 0.5f;
	Spark.Glow = 3000.0f;
	Spark.Gravity = 1.0f;
	Spark.Drag = 0.8f;
	const float BornGlow = ApexDamage::PuffGlow(Spark);
	ApexDamage::StepPuff(Spark, 0.25f);
	TestTrue(TEXT("a spark falls"), Spark.Velocity.Z < 0.0);
	TestTrue(TEXT("and cools"), ApexDamage::PuffGlow(Spark) < BornGlow);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexCarDamageRepoPartsTest, "ApexSim.Cars.Damage.RepoParts", ApexTestFlags)

bool FApexCarDamageRepoPartsTest::RunTest(const FString& Parameters)
{
	// Every repo car's damage parts cut a real piece out of its body: some
	// triangles, not most of the car, and a DRS flap's hinge on a part that
	// comes off at the back.
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content"), TEXT("cars")));
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("car.toml"), true, false);
	int32 Checked = 0;
	for (const FString& File : Files)
	{
		const FString Dir = FPaths::GetPath(File);
		const FString Car = FPaths::GetCleanFilename(Dir);
		FString Text;
		FString Error;
		FApexCarToml Toml;
		if (!FFileHelper::LoadFileToString(Text, *File) || !ApexCarToml::Parse(Text, Toml, Error) || Toml.DamageParts.IsEmpty())
		{
			continue;
		}
		FApexGlbModel Model;
		if (!TestTrue(*FString::Printf(TEXT("%s's model reads (%s)"), *Car, *Error), ApexGlb::ReadFile(FPaths::Combine(Dir, Toml.Model), Model, Error)))
		{
			continue;
		}
		const TArray<FApexDamagePartSpec> Parts = ApexCarToml::MakeDamageParts(Toml);
		TArray<FBox3f> Boxes;
		for (const FApexDamagePartSpec& Part : Parts)
		{
			Boxes.Add(FBox3f(FVector3f(Part.MinCm), FVector3f(Part.MaxCm)));
		}
		TArray<FApexGlbModel> Pieces;
		ApexGlb::SplitByBoxes(Model, Boxes, Pieces);
		const int32 Total = Model.NumTriangles();
		TestTrue(*FString::Printf(TEXT("%s keeps most of its body"), *Car), Pieces[0].NumTriangles() > Total * 0.6);
		for (int32 i = 0; i < Parts.Num(); ++i)
		{
			const int32 Tris = Pieces[i + 1].NumTriangles();
			TestTrue(*FString::Printf(TEXT("%s's %s holds bodywork (%d triangles)"), *Car, *Parts[i].Name, Tris), Tris >= 50);
			TestTrue(*FString::Printf(TEXT("%s's %s is a part, not the car (%d of %d)"), *Car, *Parts[i].Name, Tris, Total), Tris < Total / 4);
		}
		if (Toml.DrsFlap.IsPresent())
		{
			const FVector Hinge(0.0, Toml.DrsFlap.HingeForwardM * 100.0, Toml.DrsFlap.HingeUpM * 100.0);
			const bool bOnRear = Parts.ContainsByPredicate([&Hinge](const FApexDamagePartSpec& Part) {
				return Part.Zone == EApexDamageZone::Rear && Part.Box().ExpandBy(5.0).IsInsideOrOn(Hinge);
			});
			TestTrue(*FString::Printf(TEXT("%s's DRS flap is hinged on a rear part"), *Car), bOnRear);
		}
		++Checked;
	}
	if (Checked == 0)
	{
		AddInfo(FString::Printf(TEXT("no car under %s has damage parts; skipped"), *Root));
	}
	return true;
}

#endif
