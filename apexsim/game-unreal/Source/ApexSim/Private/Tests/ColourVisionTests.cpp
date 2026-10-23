#include "ApexTestCommon.h"
#include "Hud/ApexHudValue.h"
#include "UI/ApexColourVision.h"
#include "UI/ApexUIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A colour-vision deficiency at full severity, as Machado, Oliveira and Fernandes (2009) model it. */
	enum class ECvdKind : uint8 { None, Protan, Deutan, Tritan };

	/** The simulated colour, in linear RGB, clamped to the display's range. */
	FLinearColor SimulateCvd(const FLinearColor& Linear, ECvdKind Kind)
	{
		static const float Matrices[4][3][3] = {
			{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
			{{0.152286f, 1.052583f, -0.204868f}, {0.114503f, 0.786281f, 0.099216f}, {-0.003882f, -0.048116f, 1.051998f}},
			{{0.367322f, 0.860646f, -0.227968f}, {0.280085f, 0.672501f, 0.047413f}, {-0.011820f, 0.042940f, 0.968881f}},
			{{1.255528f, -0.076749f, -0.178779f}, {-0.078411f, 0.930809f, 0.147602f}, {0.004733f, 0.691367f, 0.303900f}},
		};
		const float (&M)[3][3] = Matrices[static_cast<int32>(Kind)];
		const float In[3] = {Linear.R, Linear.G, Linear.B};
		float Out[3];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			Out[Row] = FMath::Clamp(M[Row][0] * In[0] + M[Row][1] * In[1] + M[Row][2] * In[2], 0.0f, 1.0f);
		}
		return FLinearColor(Out[0], Out[1], Out[2]);
	}

	/** CIELAB (D65) of a linear sRGB colour. */
	FVector3f CvdLab(const FLinearColor& C)
	{
		const float X = (0.4124f * C.R + 0.3576f * C.G + 0.1805f * C.B) / 0.95047f;
		const float Y = 0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B;
		const float Z = (0.0193f * C.R + 0.1192f * C.G + 0.9505f * C.B) / 1.08883f;
		auto F = [](float T) { return T > 0.008856f ? FMath::Pow(T, 1.0f / 3.0f) : 7.787f * T + 16.0f / 116.0f; };
		return FVector3f(116.0f * F(Y) - 16.0f, 500.0f * (F(X) - F(Y)), 200.0f * (F(Y) - F(Z)));
	}

	/** CIE76 colour difference between two roles as a viewer with that deficiency sees them. */
	float SeenDifference(EApexColourRole A, EApexColourRole B, EApexColourVision Vision, ECvdKind Kind)
	{
		const FVector3f La = CvdLab(SimulateCvd(ApexColourVision::Role(A, Vision), Kind));
		const FVector3f Lb = CvdLab(SimulateCvd(ApexColourVision::Role(B, Vision), Kind));
		return (La - Lb).Length();
	}

	/** The roles a reader has to tell apart on the same panel (a ramp, the sector strip, the dash, a flag board). */
	const TArray<TPair<EApexColourRole, EApexColourRole>>& CvdConfusablePairs()
	{
		using R = EApexColourRole;
		static const TArray<TPair<R, R>> Pairs = {
			{R::Good, R::Bad}, {R::Good, R::Accent}, {R::Accent, R::Bad},
			{R::TyreCold, R::TyreOk}, {R::TyreCold, R::Accent}, {R::TyreCold, R::Bad},
			{R::TyreOk, R::Accent}, {R::TyreOk, R::Bad},
			{R::SessionBest, R::PersonalBest}, {R::SessionBest, R::Accent}, {R::PersonalBest, R::Accent},
			{R::FlagYellow, R::FlagBlue}, {R::FlagGreen, R::FlagYellow},
			{R::ShiftLight, R::Good}, {R::ShiftLight, R::Bad}, {R::ShiftLight, R::Accent},
		};
		return Pairs;
	}

	const TCHAR* CvdRoleName(EApexColourRole Role)
	{
		static const TCHAR* Names[] = {
			TEXT("Accent"), TEXT("Good"), TEXT("Bad"), TEXT("PersonalBest"), TEXT("SessionBest"), TEXT("TyreCold"),
			TEXT("TyreOk"), TEXT("FlagYellow"), TEXT("FlagBlue"), TEXT("FlagGreen"), TEXT("FlagRed"), TEXT("ShiftLight"),
		};
		static_assert(UE_ARRAY_COUNT(Names) == static_cast<int32>(EApexColourRole::Count), "a name per role");
		return Names[static_cast<int32>(Role)];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexColourVisionStandardTest, "ApexSim.UI.ColourVision.Standard", ApexTestFlags)
bool FApexColourVisionStandardTest::RunTest(const FString& Parameters)
{
	// The standard palette is the HUD as it always looked.
	const auto Srgb = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	const EApexColourVision S = EApexColourVision::Standard;
	TestEqual(TEXT("accent"), ApexColourVision::Role(EApexColourRole::Accent, S), ApexUI::Palette::Accent);
	TestEqual(TEXT("good"), ApexColourVision::Role(EApexColourRole::Good, S), ApexUI::Palette::Live);
	TestEqual(TEXT("bad"), ApexColourVision::Role(EApexColourRole::Bad, S), ApexUI::Palette::Error);
	TestEqual(TEXT("personal best"), ApexColourVision::Role(EApexColourRole::PersonalBest, S), ApexUI::Palette::Live);
	TestEqual(TEXT("session best"), ApexColourVision::Role(EApexColourRole::SessionBest, S), Srgb(0xB0, 0x7C, 0xE8));
	TestEqual(TEXT("tyre cold"), ApexColourVision::Role(EApexColourRole::TyreCold, S), Srgb(0x5A, 0x9B, 0xE0));
	TestEqual(TEXT("tyre ok"), ApexColourVision::Role(EApexColourRole::TyreOk, S), Srgb(0x3B, 0xB5, 0x4A));
	TestEqual(TEXT("flag yellow"), ApexColourVision::Role(EApexColourRole::FlagYellow, S), Srgb(0xFF, 0xCC, 0x00));
	TestEqual(TEXT("flag blue"), ApexColourVision::Role(EApexColourRole::FlagBlue, S), Srgb(0x1E, 0x6E, 0xFF));
	TestEqual(TEXT("flag green"), ApexColourVision::Role(EApexColourRole::FlagGreen, S), Srgb(0x14, 0x82, 0x46));

	// And it is why the switch exists: to a deuteranope the window's green
	// and the red past it are nearly one colour.
	const float Seen = SeenDifference(EApexColourRole::TyreOk, EApexColourRole::Bad, S, ECvdKind::Deutan);
	TestTrue(FString::Printf(TEXT("standard green and red merge for a deuteranope (dE %.1f)"), Seen), Seen < 10.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexColourVisionSeparationTest, "ApexSim.UI.ColourVision.Separation", ApexTestFlags)
bool FApexColourVisionSeparationTest::RunTest(const FString& Parameters)
{
	// Each palette keeps every confusable pair at least this far apart (CIE76)
	// for normal vision and for the deficiencies it is for.
	constexpr float MinDifference = 25.0f;
	struct FCase
	{
		EApexColourVision Vision;
		TArray<ECvdKind> Kinds;
	};
	const TArray<FCase> Cases = {
		{EApexColourVision::Standard, {ECvdKind::None}},
		{EApexColourVision::RedGreen, {ECvdKind::None, ECvdKind::Protan, ECvdKind::Deutan}},
		{EApexColourVision::BlueYellow, {ECvdKind::None, ECvdKind::Tritan}},
	};
	static const TCHAR* KindNames[] = {TEXT("normal"), TEXT("protan"), TEXT("deutan"), TEXT("tritan")};
	for (const FCase& Case : Cases)
	{
		for (const ECvdKind Kind : Case.Kinds)
		{
			for (const TPair<EApexColourRole, EApexColourRole>& Pair : CvdConfusablePairs())
			{
				const float Seen = SeenDifference(Pair.Key, Pair.Value, Case.Vision, Kind);
				TestTrue(FString::Printf(TEXT("%s, %s: %s vs %s dE %.1f"), ApexColourVision::Name(Case.Vision),
					KindNames[static_cast<int32>(Kind)], CvdRoleName(Pair.Key), CvdRoleName(Pair.Value), Seen), Seen >= MinDifference);
			}
		}
		// Every role reads on the HUD's dark panels.
		for (int32 Role = 0; Role < static_cast<int32>(EApexColourRole::Count); ++Role)
		{
			const float Lightness = CvdLab(ApexColourVision::Role(static_cast<EApexColourRole>(Role), Case.Vision)).X;
			TestTrue(FString::Printf(TEXT("%s: %s L* %.0f on a dark panel"), ApexColourVision::Name(Case.Vision),
				CvdRoleName(static_cast<EApexColourRole>(Role)), Lightness), Lightness >= 45.0f);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexColourVisionNamesTest, "ApexSim.UI.ColourVision.Names", ApexTestFlags)
bool FApexColourVisionNamesTest::RunTest(const FString& Parameters)
{
	EApexColourVision Vision = EApexColourVision::Standard;
	TestTrue(TEXT("red_green"), ApexColourVision::Parse(TEXT("red_green"), Vision) && Vision == EApexColourVision::RedGreen);
	TestTrue(TEXT("Deuteranopia"), ApexColourVision::Parse(TEXT("Deuteranopia"), Vision) && Vision == EApexColourVision::RedGreen);
	TestTrue(TEXT("protanopia"), ApexColourVision::Parse(TEXT("protanopia"), Vision) && Vision == EApexColourVision::RedGreen);
	TestTrue(TEXT("blue-yellow"), ApexColourVision::Parse(TEXT("blue-yellow"), Vision) && Vision == EApexColourVision::BlueYellow);
	TestTrue(TEXT("tritanopia"), ApexColourVision::Parse(TEXT("tritanopia"), Vision) && Vision == EApexColourVision::BlueYellow);
	TestTrue(TEXT("0"), ApexColourVision::Parse(TEXT("0"), Vision) && Vision == EApexColourVision::Standard);
	TestFalse(TEXT("purple"), ApexColourVision::Parse(TEXT("purple"), Vision));
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const EApexColourVision Each = static_cast<EApexColourVision>(Index);
		EApexColourVision Back = EApexColourVision::Standard;
		TestTrue(FString::Printf(TEXT("%s round trip"), ApexColourVision::Name(Each)),
			ApexColourVision::Parse(ApexColourVision::Name(Each), Back) && Back == Each);
	}

	// The HUD's colour names follow the palette current when they are read;
	// the menu-only names do not move.
	const EApexColourVision Before = ApexColourVision::Current();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const EApexColourVision Each = static_cast<EApexColourVision>(Index);
		ApexColourVision::SetCurrent(Each);
		struct FNamed { const TCHAR* Name; EApexColourRole Role; };
		const FNamed Named[] = {
			{TEXT("accent"), EApexColourRole::Accent}, {TEXT("live"), EApexColourRole::Good},
			{TEXT("error"), EApexColourRole::Bad}, {TEXT("personal_best"), EApexColourRole::PersonalBest},
			{TEXT("session_best"), EApexColourRole::SessionBest}, {TEXT("tyre_cold"), EApexColourRole::TyreCold},
			{TEXT("tyre_ok"), EApexColourRole::TyreOk}, {TEXT("flag_yellow"), EApexColourRole::FlagYellow},
			{TEXT("flag_blue"), EApexColourRole::FlagBlue}, {TEXT("flag_green"), EApexColourRole::FlagGreen},
			{TEXT("flag_red"), EApexColourRole::FlagRed},
		};
		for (const FNamed& Entry : Named)
		{
			FLinearColor Parsed;
			TestTrue(FString::Printf(TEXT("%s parses"), Entry.Name), ApexHudColour::Parse(Entry.Name, Parsed));
			TestEqual(FString::Printf(TEXT("%s in %s"), Entry.Name, ApexColourVision::Name(Each)), Parsed, ApexColourVision::Role(Entry.Role, Each));
		}
		FLinearColor Surface;
		TestTrue(TEXT("surface parses"), ApexHudColour::Parse(TEXT("surface"), Surface));
		TestEqual(FString::Printf(TEXT("surface in %s"), ApexColourVision::Name(Each)), Surface, ApexUI::Palette::Surface);
	}
	ApexColourVision::SetCurrent(Before);
	return true;
}

#endif
