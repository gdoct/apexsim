#include "Track/ApexGroundMaterials.h"

#include "Algo/BinarySearch.h"

namespace
{
	/** Prefix the exporter gives every lateral run-off band. */
	const TCHAR* const kBandPrefix = TEXT("surface_");
}	 // namespace

ApexGround::FSurfaceLook ApexGround::LookFor(const FString& Family, const FString& Key)
{
	// Everything paved samples the asphalt set, the painted markings
	// included: a white line is paint over the same aggregate, so it wants
	// the same grain underneath, only flatter and less rough.
	if (Family == TEXT("road") || Family == TEXT("pit_lane"))
	{
		return {TEXT("asphalt"), TextureTileM, 1.0f, 0.5f, 0.0f};
	}
	if (Family == TEXT("marking"))
	{
		return {TEXT("asphalt"), TextureTileM, 0.35f, 0.2f, 0.0f};
	}
	if (Family == TEXT("curb"))
	{
		return {TEXT("kerb"), TextureTileM, 0.8f, 0.5f, 0.0f};
	}
	if (Family == TEXT("structure"))
	{
		return {TEXT("concrete"), TextureTileM, 0.7f, 0.4f, 0.0f};
	}
	if (Family == TEXT("surface"))
	{
		if (Key == TEXT("surface_gravel"))
		{
			return {TEXT("gravel"), TextureTileM, 1.0f, 0.5f, 1.0f};
		}
		if (Key == TEXT("surface_sand"))
		{
			return {TEXT("sand"), TextureTileM, 1.0f, 0.5f, 1.0f};
		}
		if (Key == TEXT("surface_concrete"))
		{
			return {TEXT("concrete"), TextureTileM, 0.7f, 0.4f, 0.8f};
		}
		if (Key == TEXT("surface_asphalt_runoff"))
		{
			return {TEXT("asphalt"), TextureTileM, 1.0f, 0.5f, 0.8f};
		}
		if (Key == TEXT("surface_astroturf"))
		{
			// Half scale: the fibres are three centimetres apart on the real
			// thing, and a strip is only about a metre wide, so shown at the
			// authored size one repeat covers the whole strip.
			return {TEXT("astroturf"), TextureTileM * 0.5f, 0.8f, 0.4f, 0.6f};
		}
		if (!IsSurfaceBand(Key))
		{
			// The terrain. Stretched, because it runs for kilometres and a
			// two-metre repeat out to the horizon is a moire pattern however
			// good the blend is, and given no fringe because it has no edges
			// — it is what everything else is fringed against.
			return {TEXT("grass"), TextureTileM * 2.0f, 1.0f, 0.5f, 0.0f};
		}
		return {TEXT("grass"), TextureTileM, 1.0f, 0.5f, 1.0f};
	}
	return {TEXT(""), 0.0f, 0.0f, 0.0f, 0.0f};
}

TArray<FString> ApexGround::AllSets()
{
	// The `GROUND_SLOTS` table in apex_tex.py, in the order it is written.
	return {TEXT("asphalt"), TEXT("grass"), TEXT("gravel"), TEXT("sand"), TEXT("concrete"),
		TEXT("astroturf"), TEXT("kerb")};
}

FString ApexGround::TexturePath(const FString& Set, const TCHAR* Map)
{
	return FString(TexturesRoot) / (FString(TexturePrefix) + Set + TEXT("_") + Map);
}

bool ApexGround::IsSurfaceBand(const FString& Key)
{
	return Key.StartsWith(kBandPrefix, ESearchCase::CaseSensitive);
}

float ApexGround::EdgeFactor(float FromInnerM, float WidthM)
{
	const float Span = FMath::Min(EdgeSpanM, WidthM * EdgeMaxFraction);
	if (!(Span > 0.0f))
	{
		// A degenerate section — one column, or a band that pinched out.
		// Treated as all interior, which leaves it exactly as it was.
		return 1.0f;
	}
	return FMath::Clamp(FromInnerM / Span, 0.0f, 1.0f);
}

TArray<float> ApexGround::EdgeFactors(TArrayView<const FVector2f> UVs,
	TArrayView<const FVector3f> Positions, TArrayView<const FCenterSample> Centerline, float LapM)
{
	TArray<float> Factors;
	Factors.Init(1.0f, UVs.Num());
	if (Centerline.Num() == 0 || Positions.Num() != UVs.Num())
	{
		return Factors;
	}

	// The centerline sample nearest a station, by binary search: the
	// samples come in station order from the exporter.
	auto CenterAt = [&](float StationM) -> const FCenterSample& {
		if (LapM > 0.0f)
		{
			StationM = FMath::Fmod(StationM, LapM);
			if (StationM < 0.0f)
			{
				StationM += LapM;
			}
		}
		const int32 Upper = Algo::LowerBoundBy(Centerline, StationM, &FCenterSample::StationM);
		if (Upper <= 0)
		{
			return Centerline[0];
		}
		if (Upper >= Centerline.Num())
		{
			return Centerline.Last();
		}
		const FCenterSample& Before = Centerline[Upper - 1];
		const FCenterSample& After = Centerline[Upper];
		return (StationM - Before.StationM) <= (After.StationM - StationM) ? Before : After;
	};

	// One cross-section of one band, keyed by its station and its side of
	// the road.
	struct FSection
	{
		float MinV = TNumericLimits<float>::Max();
		float MaxV = -TNumericLimits<float>::Max();
		float DistanceAtMin = 0.0f;
		float DistanceAtMax = 0.0f;
	};
	TMap<int64, FSection> Sections;
	TArray<int64> Keys;
	Keys.SetNumUninitialized(UVs.Num());
	for (int32 i = 0; i < UVs.Num(); ++i)
	{
		const FCenterSample& Center = CenterAt(UVs[i].X);
		const FVector2f Offset = FVector2f(Positions[i].X, Positions[i].Y) - Center.Location;
		const int64 Side = FVector2f::DotProduct(Offset, Center.Across) >= 0.0f ? 1 : 0;
		const int64 Key = (int64(FMath::RoundToInt(UVs[i].X * 1000.0f)) << 1) | Side;
		Keys[i] = Key;

		const float Distance = Offset.Size();
		FSection& Section = Sections.FindOrAdd(Key);
		if (UVs[i].Y < Section.MinV)
		{
			Section.MinV = UVs[i].Y;
			Section.DistanceAtMin = Distance;
		}
		if (UVs[i].Y > Section.MaxV)
		{
			Section.MaxV = UVs[i].Y;
			Section.DistanceAtMax = Distance;
		}
	}

	for (int32 i = 0; i < UVs.Num(); ++i)
	{
		const FSection& Section = Sections.FindChecked(Keys[i]);
		const float Width = Section.MaxV - Section.MinV;
		const bool bInnerAtMin = Section.DistanceAtMin <= Section.DistanceAtMax;
		const float FromInner = bInnerAtMin ? UVs[i].Y - Section.MinV : Section.MaxV - UVs[i].Y;
		Factors[i] = EdgeFactor(FromInner, Width);
	}
	return Factors;
}
