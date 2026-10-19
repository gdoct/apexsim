#include "Track/ApexGroundMaterials.h"
#include "ApexTestCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGroundLookTest, "ApexSim.Track.Ground.Looks", ApexTestFlags)

bool FApexGroundLookTest::RunTest(const FString& Parameters)
{
	// The exporter's families, each on the set it should sample.
	TestEqual(TEXT("road is asphalt"),
		FString(ApexGround::LookFor(TEXT("road"), TEXT("road")).Set), FString(TEXT("asphalt")));
	TestEqual(TEXT("the pit lane is asphalt too"),
		FString(ApexGround::LookFor(TEXT("pit_lane"), TEXT("pit_lane")).Set),
		FString(TEXT("asphalt")));
	TestEqual(TEXT("wear bands are road, whatever they are called"),
		FString(ApexGround::LookFor(TEXT("road"), TEXT("wear_core")).Set),
		FString(TEXT("asphalt")));
	TestEqual(TEXT("kerbs get the paint"),
		FString(ApexGround::LookFor(TEXT("curb"), TEXT("curb_red_white")).Set),
		FString(TEXT("kerb")));
	TestEqual(TEXT("structures are concrete"),
		FString(ApexGround::LookFor(TEXT("structure"), TEXT("structure")).Set),
		FString(TEXT("concrete")));
	TestEqual(TEXT("gravel traps are gravel"),
		FString(ApexGround::LookFor(TEXT("surface"), TEXT("surface_gravel")).Set),
		FString(TEXT("gravel")));
	TestEqual(TEXT("paved run-off is asphalt, not a surface of its own"),
		FString(ApexGround::LookFor(TEXT("surface"), TEXT("surface_asphalt_runoff")).Set),
		FString(TEXT("asphalt")));
	// A kind the exporter might add tomorrow still lands on something.
	TestEqual(TEXT("an unknown surface kind falls back to grass"),
		FString(ApexGround::LookFor(TEXT("surface"), TEXT("surface_meadow")).Set),
		FString(TEXT("grass")));
	TestTrue(TEXT("an unknown family asks for no texture"),
		FString(ApexGround::LookFor(TEXT("hologram"), TEXT("whatever")).Set).IsEmpty());

	// Every set a look names has to be one the baker actually writes, or
	// the import leaves a family on the parent's default and nobody notices.
	const TArray<FString> Sets = ApexGround::AllSets();
	const TCHAR* const Families[] = {TEXT("road"), TEXT("pit_lane"), TEXT("curb"), TEXT("marking"),
		TEXT("structure"), TEXT("surface")};
	const TCHAR* const Keys[] = {TEXT("road"), TEXT("ground"), TEXT("surface_grass"),
		TEXT("surface_gravel"), TEXT("surface_sand"), TEXT("surface_concrete"),
		TEXT("surface_astroturf"), TEXT("surface_asphalt_runoff"), TEXT("curb_yellow_black")};
	for (const TCHAR* Family : Families)
	{
		for (const TCHAR* Key : Keys)
		{
			const ApexGround::FSurfaceLook Look = ApexGround::LookFor(Family, Key);
			TestTrue(FString::Printf(TEXT("%s/%s names a baked set"), Family, Key),
				Sets.Contains(FString(Look.Set)));
			TestTrue(FString::Printf(TEXT("%s/%s tiles at a sane size"), Family, Key),
				Look.TileM > 0.1f && Look.TileM < 100.0f);
		}
	}

	// The terrain is what the bands are fringed against, so it is stretched
	// and takes no fringe of its own.
	const ApexGround::FSurfaceLook Ground = ApexGround::LookFor(TEXT("surface"), TEXT("ground"));
	TestFalse(TEXT("the terrain is not a band"), ApexGround::IsSurfaceBand(TEXT("ground")));
	TestEqual(TEXT("the terrain takes no fringe"), Ground.EdgeBlend, 0.0f);
	TestTrue(TEXT("the terrain is tiled larger than a band"),
		Ground.TileM > ApexGround::LookFor(TEXT("surface"), TEXT("surface_grass")).TileM);
	TestTrue(TEXT("grass bands are fringed"),
		ApexGround::IsSurfaceBand(TEXT("surface_grass"))
			&& ApexGround::LookFor(TEXT("surface"), TEXT("surface_grass")).EdgeBlend > 0.0f);
	TestEqual(TEXT("the road takes no fringe"),
		ApexGround::LookFor(TEXT("road"), TEXT("road")).EdgeBlend, 0.0f);

	TestEqual(TEXT("asset path"), ApexGround::TexturePath(TEXT("grass"), TEXT("nrm")),
		FString(TEXT("/Game/Ground/T_ground_grass_nrm")));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGroundEdgeTest, "ApexSim.Track.Ground.EdgeFringe", ApexTestFlags)

bool FApexGroundEdgeTest::RunTest(const FString& Parameters)
{
	// A wide band: only the last EdgeSpanM on the road side is fringe.
	constexpr float Wide = 20.0f;
	TestEqual(TEXT("the road-facing edge is all fringe"), ApexGround::EdgeFactor(0.0f, Wide), 0.0f);
	TestEqual(TEXT("the ramp ends at the span"),
		ApexGround::EdgeFactor(ApexGround::EdgeSpanM, Wide), 1.0f);
	TestEqual(TEXT("half way in is half way up the ramp"),
		ApexGround::EdgeFactor(ApexGround::EdgeSpanM * 0.5f, Wide), 0.5f);
	TestEqual(TEXT("the outer edge is untouched"), ApexGround::EdgeFactor(Wide, Wide), 1.0f);

	// A narrow band — an astroturf strip is about a metre — must keep most
	// of itself as the surface it is meant to be.
	constexpr float Narrow = 1.0f;
	TestEqual(TEXT("a narrow band is still fringed at its edge"),
		ApexGround::EdgeFactor(0.0f, Narrow), 0.0f);
	TestEqual(TEXT("a narrow band's fringe stops at the cap"),
		ApexGround::EdgeFactor(Narrow * ApexGround::EdgeMaxFraction, Narrow), 1.0f);

	// A pinched-out section is left exactly as it was.
	TestEqual(TEXT("zero width is all interior"), ApexGround::EdgeFactor(0.0f, 0.0f), 1.0f);

	// The case the exporter actually produces: a straight road along +X,
	// grass on both sides merged into one mesh, each profile ordered right
	// to left before `v` is measured. So the left band's `v` starts at its
	// inner edge (y = 6) and the right band's at its outer edge (y = -16).
	TArray<ApexGround::FCenterSample> Center;
	for (int32 i = 0; i <= 4; ++i)
	{
		ApexGround::FCenterSample& Sample = Center.AddDefaulted_GetRef();
		Sample.StationM = 50.0f * i;
		Sample.Location = FVector2f(5000.0f * i, 0.0f);
		Sample.Across = FVector2f(0.0f, 1.0f);
	}
	const float U = 100.0f;
	const float X = U * 100.0f;
	const TArray<FVector2f> UVs = {
		{U, 0.0f}, {U, 10.0f}, {U, 20.0f},	  // left band, y = 6 .. 26 m
		{U, 0.0f}, {U, 5.0f}, {U, 10.0f},	  // right band, y = -16 .. -6 m
	};
	const TArray<FVector3f> Positions = {
		{X, 600.0f, 0.0f}, {X, 1600.0f, 0.0f}, {X, 2600.0f, 0.0f},
		{X, -1600.0f, 0.0f}, {X, -1100.0f, 0.0f}, {X, -600.0f, 0.0f},
	};
	const TArray<float> Factors = ApexGround::EdgeFactors(UVs, Positions, Center, 0.0f);
	TestEqual(TEXT("one factor per vertex"), Factors.Num(), UVs.Num());
	TestEqual(TEXT("left band: inner edge fringed"), Factors[0], 0.0f);
	TestEqual(TEXT("left band: middle clean"), Factors[1], 1.0f);
	TestEqual(TEXT("left band: outer edge clean"), Factors[2], 1.0f);
	TestEqual(TEXT("right band: outer edge clean, though v starts there"), Factors[3], 1.0f);
	TestEqual(TEXT("right band: middle clean"), Factors[4], 1.0f);
	TestEqual(TEXT("right band: inner edge fringed, though v ends there"), Factors[5], 0.0f);

	// Without a centerline nothing can be told apart, so nothing is fringed.
	const TArray<float> Blind = ApexGround::EdgeFactors(UVs, Positions, {}, 0.0f);
	TestTrue(TEXT("no centerline, no fringe"),
		Blind.Num() == UVs.Num() && !Blind.ContainsByPredicate([](float F) { return F < 1.0f; }));

	// A span through start/finish keeps counting past the lap; the lookup
	// has to wrap rather than pin to the last sample.
	const TArray<FVector2f> Wrapped = {{U + 200.0f, 0.0f}, {U + 200.0f, 20.0f}};
	const TArray<FVector3f> WrappedAt = {{X, 600.0f, 0.0f}, {X, 2600.0f, 0.0f}};
	const TArray<float> Lapped = ApexGround::EdgeFactors(Wrapped, WrappedAt, Center, 200.0f);
	TestEqual(TEXT("wrapped station: inner edge found"), Lapped[0], 0.0f);
	TestEqual(TEXT("wrapped station: outer edge clean"), Lapped[1], 1.0f);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
