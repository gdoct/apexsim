#include "ApexTestCommon.h"
#include "HAL/FileManager.h"
#include "Hud/ApexHudComponent.h"
#include "Hud/ApexHudData.h"
#include "Hud/ApexHudExpression.h"
#include "Hud/ApexHudLayout.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Evaluate Source against Data (and an optional repeat row). */
	FApexHudValue HudEval(const FString& Source, const FApexHudData* Data = nullptr, const FApexHudRecord* Item = nullptr, int32 Index = -1)
	{
		FString Error;
		const TSharedPtr<const FApexHudExpr> Expr = FApexHudExpr::Compile(Source, Error);
		if (!Expr)
		{
			return FApexHudValue::Of(FString(TEXT("COMPILE ERROR: ")) + Error);
		}
		FApexHudScope Scope;
		Scope.Data = Data;
		Scope.Item = Item;
		Scope.Index = Index;
		return Expr->Evaluate(Scope);
	}

	FString HudText(const FString& Source, const FApexHudData* Data = nullptr)
	{
		return HudEval(Source, Data).AsString();
	}

	bool HudCompiles(const FString& Source)
	{
		FString Error;
		return FApexHudExpr::Compile(Source, Error).IsValid();
	}

	FString HudRepoPath(const TCHAR* Relative)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), Relative));
	}

	/** A scratch folder under Saved, removed by the caller. */
	FString HudScratchDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HudTests"), FGuid::NewGuid().ToString()));
	}

	FApexCarTelemetry HudCar(int32 Index, int32 Lap, float Station, float SpeedMps)
	{
		FApexCarTelemetry Car;
		Car.CarIndex = Index;
		Car.CurrentLap = Lap;
		Car.TrackProgress = Station;
		Car.SpeedMps = SpeedMps;
		return Car;
	}

	/** Two points within a hundredth of a unit (FSlateRect hands back float vectors). */
	bool HudSame(const FVector2D& A, const FVector2D& B)
	{
		return A.Equals(B, 0.01);
	}

	const FApexHudValue& HudValue(const FApexHudData& Data, const TCHAR* Name)
	{
		static const FApexHudValue Missing = FApexHudValue::Of(TEXT("<missing>"));
		const FApexHudValue* Value = Data.Find(FName(Name));
		return Value ? *Value : Missing;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudExprBasicsTest, "ApexSim.Hud.Expr.Basics", ApexTestFlags)

bool FApexHudExprBasicsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("precedence"), HudEval(TEXT("1 + 2 * 3")).AsNumber(), 7.0);
	TestEqual(TEXT("parentheses"), HudEval(TEXT("(1 + 2) * 3")).AsNumber(), 9.0);
	TestEqual(TEXT("unary minus"), HudEval(TEXT("-2 + 5")).AsNumber(), 3.0);
	TestEqual(TEXT("modulo"), HudEval(TEXT("7 % 3")).AsNumber(), 1.0);
	TestEqual(TEXT("division by zero is zero"), HudEval(TEXT("1 / 0")).AsNumber(), 0.0);
	TestEqual(TEXT("text joins"), HudText(TEXT("'LAP ' + 3")), FString(TEXT("LAP 3")));
	TestEqual(TEXT("whole numbers print bare"), HudText(TEXT("12.0")), FString(TEXT("12")));
	TestEqual(TEXT("fractions to three places"), HudText(TEXT("1 / 4")), FString(TEXT("0.25")));
	TestTrue(TEXT("comparison"), HudEval(TEXT("3 >= 3 && 2 < 3")).AsBool());
	TestTrue(TEXT("word operators"), HudEval(TEXT("not false and (true or false)")).AsBool());
	TestTrue(TEXT("text equality"), HudEval(TEXT("'ok' == \"ok\"")).AsBool());
	TestEqual(TEXT("ternary is right-associative"), HudText(TEXT("false ? 'a' : true ? 'b' : 'c'")), FString(TEXT("b")));

	// A name nobody fills is null: false, 0 and "" in every context.
	TestTrue(TEXT("unknown name is null"), HudEval(TEXT("no.such.name")).IsNone());
	TestFalse(TEXT("null is false"), HudEval(TEXT("no.such.name")).AsBool());
	TestFalse(TEXT("has(null)"), HudEval(TEXT("has(no.such.name)")).AsBool());
	TestTrue(TEXT("null == null"), HudEval(TEXT("no.such.name == null")).AsBool());
	TestFalse(TEXT("null != 0 for equality"), HudEval(TEXT("no.such.name == 0")).AsBool());

	// Formatting: the shapes the HUD shows.
	TestEqual(TEXT("fmt_time"), HudText(TEXT("fmt_time(92.104)")), FString(TEXT("1:32.104")));
	TestEqual(TEXT("fmt_time without a time"), HudText(TEXT("fmt_time(0)")), FString(TEXT("--:--.---")));
	TestEqual(TEXT("fmt_split"), HudText(TEXT("fmt_split(27.4312)")), FString(TEXT("27.431")));
	TestEqual(TEXT("fmt_gap"), HudText(TEXT("fmt_gap(0.4)")), FString(TEXT("+0.400")));
	TestEqual(TEXT("fmt_gap unsigned"), HudText(TEXT("fmt_gap(0.4, false)")), FString(TEXT("0.400")));
	TestEqual(TEXT("fmt_gap of nothing"), HudText(TEXT("fmt_gap(null)")), FString(TEXT("—")));
	TestEqual(TEXT("fmt_delta"), HudText(TEXT("fmt_delta(-0.25)")), FString(TEXT("-0.250")));
	TestEqual(TEXT("fmt_clock"), HudText(TEXT("fmt_clock(83.2)")), FString(TEXT("1:24")));
	TestEqual(TEXT("fmt_clock with hours"), HudText(TEXT("fmt_clock(86400)")), FString(TEXT("24:00:00")));
	TestEqual(TEXT("fmt_clock out of time"), HudText(TEXT("fmt_clock(0)")), FString(TEXT("0:00")));
	TestEqual(TEXT("fmt_clock of nothing"), HudText(TEXT("fmt_clock(null)")), FString(TEXT("--:--")));
	TestEqual(TEXT("fmt"), HudText(TEXT("fmt(12.345, 1)")), FString(TEXT("12.3")));
	TestEqual(TEXT("fmt of nothing"), HudText(TEXT("fmt(null)")), FString(TEXT("—")));
	TestEqual(TEXT("round"), HudEval(TEXT("round(2.5)")).AsNumber(), 3.0);
	TestEqual(TEXT("clamp"), HudEval(TEXT("clamp(12, 0, 10)")).AsNumber(), 10.0);
	TestEqual(TEXT("min of many"), HudEval(TEXT("min(4, 2, 9)")).AsNumber(), 2.0);
	TestEqual(TEXT("upper"), HudText(TEXT("upper('you')")), FString(TEXT("YOU")));
	TestEqual(TEXT("switch hits"), HudText(TEXT("switch('hot', 'cold', 1, 'hot', 2, 3)")), FString(TEXT("2")));
	TestEqual(TEXT("switch default"), HudText(TEXT("switch('x', 'cold', 1, 3)")), FString(TEXT("3")));
	TestTrue(TEXT("switch without default is null"), HudEval(TEXT("switch('x', 'cold', 1)")).IsNone());

	// Colours: names, hex, and arithmetic on them.
	FLinearColor Colour;
	TestTrue(TEXT("a name is a colour"), HudEval(TEXT("'accent'")).AsColour(Colour));
	TestTrue(TEXT("hex is a colour"), HudEval(TEXT("'#FF000080'")).AsColour(Colour) && FMath::IsNearlyEqual(Colour.A, 128.0f / 255.0f, 0.01f));
	TestFalse(TEXT("a typo is not"), HudEval(TEXT("'acent'")).AsColour(Colour));
	TestTrue(TEXT("mix"), HudEval(TEXT("mix('black', 'white', 0.5)")).AsColour(Colour) && FMath::IsNearlyEqual(Colour.R, 0.5f));
	TestTrue(TEXT("ramp below"), HudEval(TEXT("ramp(-5, 0, 'black', 10, 'white')")).AsColour(Colour) && Colour.R == 0.0f);
	TestTrue(TEXT("ramp middle"), HudEval(TEXT("ramp(5, 0, 'black', 10, 'white')")).AsColour(Colour) && FMath::IsNearlyEqual(Colour.R, 0.5f));
	TestTrue(TEXT("ramp above"), HudEval(TEXT("ramp(50, 0, 'black', 10, 'white')")).AsColour(Colour) && Colour.R == 1.0f);
	TestTrue(TEXT("alpha"), HudEval(TEXT("alpha('white', 0.25)")).AsColour(Colour) && FMath::IsNearlyEqual(Colour.A, 0.25f));

	// Data, rows and the repeat index.
	FApexHudData Data;
	Data.Set(TEXT("car.speed_kph"), 212.4);
	Data.Set(TEXT("session.mode_name"), TEXT("Race"));
	FApexHudRecord Row;
	Row.Add(TEXT("name"), FApexHudValue::Of(TEXT("Lewis")));
	TestEqual(TEXT("a data point"), HudEval(TEXT("round(car.speed_kph)"), &Data).AsNumber(), 212.0);
	TestEqual(TEXT("an item field"), HudEval(TEXT("upper(item.name)"), &Data, &Row).AsString(), FString(TEXT("LEWIS")));
	TestEqual(TEXT("index"), HudEval(TEXT("index * 2"), &Data, &Row, 3).AsNumber(), 6.0);

	// Constant folding is what lets the host apply a fixed attribute once.
	FString Error;
	TestTrue(TEXT("literal arithmetic is constant"), FApexHudExpr::Compile(TEXT("1 + 2"), Error)->IsConstant());
	TestFalse(TEXT("a data point is not"), FApexHudExpr::Compile(TEXT("car.rpm + 1"), Error)->IsConstant());
	TArray<FString> Names;
	FApexHudExpr::Compile(TEXT("item.name + car.rpm + index"), Error)->CollectIdentifiers(Names);
	TestTrue(TEXT("identifiers collected"), Names.Contains(TEXT("item.name")) && Names.Contains(TEXT("car.rpm")) && Names.Contains(TEXT("index")));

	// What does not parse says why.
	TestFalse(TEXT("dangling operator"), HudCompiles(TEXT("1 +")));
	TestFalse(TEXT("unknown function"), HudCompiles(TEXT("nope(1)")));
	TestFalse(TEXT("wrong argument count"), HudCompiles(TEXT("clamp(1, 2)")));
	TestFalse(TEXT("unclosed text"), HudCompiles(TEXT("'abc")));
	TestFalse(TEXT("bare item"), HudCompiles(TEXT("item")));
	TestFalse(TEXT("ramp needs pairs"), HudCompiles(TEXT("ramp(1, 0, 'red', 2)")));
	TestFalse(TEXT("ternary without else"), HudCompiles(TEXT("true ? 1")));
	FApexHudExpr::Compile(TEXT("1 + )"), Error);
	TestTrue(TEXT("error names the column"), Error.Contains(TEXT("column 5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudExprTemplateTest, "ApexSim.Hud.Expr.Template", ApexTestFlags)

bool FApexHudExprTemplateTest::RunTest(const FString& Parameters)
{
	FApexHudData Data;
	Data.Set(TEXT("lap.display"), 3);
	Data.Set(TEXT("lap.limit"), 12);

	auto Render = [&Data](const FString& Source)
	{
		FString Error;
		const TSharedPtr<const FApexHudExpr> Expr = FApexHudExpr::CompileTemplate(Source, Error);
		FApexHudScope Scope;
		Scope.Data = &Data;
		return Expr ? Expr->Evaluate(Scope).AsString() : TEXT("ERROR: ") + Error;
	};

	TestEqual(TEXT("holes"), Render(TEXT("LAP {lap.display}/{lap.limit}")), FString(TEXT("LAP 3/12")));
	TestEqual(TEXT("expression in a hole"), Render(TEXT("{lap.limit - lap.display} to go")), FString(TEXT("9 to go")));
	TestEqual(TEXT("a brace inside quotes"), Render(TEXT("{'}'}")), FString(TEXT("}")));
	TestEqual(TEXT("escaped braces"), Render(TEXT("{{lap}}")), FString(TEXT("{lap}")));
	TestEqual(TEXT("plain text"), Render(TEXT("Standings")), FString(TEXT("Standings")));

	FString Error;
	TestTrue(TEXT("plain text is constant"), FApexHudExpr::CompileTemplate(TEXT("Standings"), Error)->IsConstant());
	TestFalse(TEXT("unclosed hole"), FApexHudExpr::CompileTemplate(TEXT("LAP {lap.display"), Error).IsValid());
	TestFalse(TEXT("stray closing brace"), FApexHudExpr::CompileTemplate(TEXT("LAP }"), Error).IsValid());
	TestFalse(TEXT("bad expression in a hole"), FApexHudExpr::CompileTemplate(TEXT("{1 +}"), Error).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudJsonExtrasTest, "ApexSim.Hud.Json.Extras", ApexTestFlags)

bool FApexHudJsonExtrasTest::RunTest(const FString& Parameters)
{
	const FString Source = TEXT(
		"// a heading comment\n"
		"{\n"
		"  \"a\": \"http://not-a-comment\", /* block */\n"
		"  \"b\": [1, 2, ],\n"
		"  \"c\": \"a, }\",\n"
		"}\n");
	const FString Clean = ApexHud::StripJsonExtras(Source);
	TestTrue(TEXT("URL kept"), Clean.Contains(TEXT("http://not-a-comment")));
	TestFalse(TEXT("line comment gone"), Clean.Contains(TEXT("heading")));
	TestFalse(TEXT("block comment gone"), Clean.Contains(TEXT("block")));
	TestTrue(TEXT("comma inside a string kept"), Clean.Contains(TEXT("\"a, }\"")));
	TestFalse(TEXT("trailing comma in array gone"), Clean.Contains(TEXT("2, ]")));
	TestEqual(TEXT("line count kept"), Clean.Len() - Clean.Replace(TEXT("\n"), TEXT("")).Len(), Source.Len() - Source.Replace(TEXT("\n"), TEXT("")).Len());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudComponentParseTest, "ApexSim.Hud.Component.Parse", ApexTestFlags)

bool FApexHudComponentParseTest::RunTest(const FString& Parameters)
{
	{
		FApexHudComponentDef Component;
		FApexHudLoadReport Report;
		const bool bOk = ApexHud::ParseComponent(TEXT(R"({
			"name": "Speed", "region": "bottom", "order": 3, "margin": [1, 2],
			"root": { "type": "panel", "padding": 8, "background": "surface",
				"children": [
					{ "type": "text", "text": "{fmt(car.speed)} KM/H", "color": "=car.speed > 300 ? 'error' : 'text'", "bold": true },
					{ "type": "rect", "repeat": 10, "width": 4, "height": 4, "color": "#3FC46B" },
					{ "type": "label", "repeat": { "list": "standings", "max": 3, "focus": "is_local" }, "text": "{item.name}" },
				]
			}
		})"), TEXT("C:/hud/speed"), Component, Report);
		TestTrue(TEXT("parses"), bOk);
		TestEqual(TEXT("no errors"), Report.Errors.Num(), 0);
		TestEqual(TEXT("no warnings"), Report.Warnings.Num(), 0);
		TestEqual(TEXT("id from the folder"), Component.Id, FString(TEXT("speed")));
		TestEqual(TEXT("region"), Component.Region, FString(TEXT("bottom")));
		TestEqual(TEXT("order"), Component.Order, 3);
		TestEqual(TEXT("margin [h, v]"), Component.Margin, FMargin(1.0f, 2.0f));
		TestEqual(TEXT("children"), Component.Root.Children.Num(), 3);
		TestTrue(TEXT("dynamic text"), Component.Root.Children[0].Text.IsDynamic());
		TestFalse(TEXT("fixed background"), Component.Root.Background.IsDynamic());
		TestEqual(TEXT("count repeat"), Component.Root.Children[1].Repeat.Slots(), 10);
		TestEqual(TEXT("list repeat"), Component.Root.Children[2].Repeat.Slots(), 3);
		TestEqual(TEXT("a label is mono capitals"), Component.Root.Children[2].FontFace, FString(TEXT("mono")));

		TArray<FString> Warnings;
		ApexHud::CheckDataNames(Component, Warnings);
		TestEqual(TEXT("every name is published"), Warnings.Num(), 0);
		for (const FString& Warning : Warnings)
		{
			AddInfo(Warning);
		}
	}

	auto Errors = [](const FString& Json)
	{
		FApexHudComponentDef Component;
		FApexHudLoadReport Report;
		ApexHud::ParseComponent(Json, TEXT("C:/hud/x"), Component, Report);
		return Report;
	};
	TestTrue(TEXT("no root"), Errors(TEXT(R"({ "region": "top" })")).Errors.Num() > 0);
	TestTrue(TEXT("bad region"), Errors(TEXT(R"({ "region": "middle", "root": { "type": "row" } })")).Errors.Num() > 0);
	TestTrue(TEXT("bad type"), Errors(TEXT(R"({ "root": { "type": "button" } })")).Errors.Num() > 0);
	TestTrue(TEXT("bad colour"), Errors(TEXT(R"({ "root": { "type": "rect", "width": 1, "height": 1, "color": "acent" } })")).Errors.Num() > 0);
	TestTrue(TEXT("bad expression"), Errors(TEXT(R"({ "root": { "type": "text", "text": "=1 +" } })")).Errors.Num() > 0);
	TestTrue(TEXT("children on a leaf"), Errors(TEXT(R"({ "root": { "type": "text", "text": "a", "children": [] } })")).Errors.Num() > 0);
	TestTrue(TEXT("unknown list"), Errors(TEXT(R"({ "root": { "type": "row", "children": [ { "type": "label", "text": "a", "repeat": { "list": "cars" } } ] } })")).Errors.Num() > 0);
	TestTrue(TEXT("not JSON"), Errors(TEXT("{ root: ")).Errors.Num() > 0);
	TestTrue(TEXT("a visible that is not an expression"), Errors(TEXT(R"({ "root": { "type": "row", "visible": "car.present" } })")).Errors.Num() > 0);

	const FApexHudLoadReport Typo = Errors(TEXT(R"({ "root": { "type": "text", "txt": "a", "text": "b" } })"));
	TestEqual(TEXT("unknown key is a warning"), Typo.Warnings.Num(), 1);
	TestEqual(TEXT("not an error"), Typo.Errors.Num(), 0);

	FApexHudComponentDef Misspelt;
	FApexHudLoadReport Report;
	ApexHud::ParseComponent(TEXT(R"({ "root": { "type": "row", "children": [
		{ "type": "text", "text": "{car.sped}" },
		{ "type": "text", "text": "{item.name}" },
		{ "type": "text", "repeat": 2, "text": "{index}" },
		{ "type": "text", "repeat": { "list": "tyres" }, "text": "{item.nmae}" }
	] } })"), TEXT("C:/hud/x"), Misspelt, Report);
	TArray<FString> Warnings;
	ApexHud::CheckDataNames(Misspelt, Warnings);
	TestEqual(TEXT("a misspelt data point, an item outside a list and a misspelt field"), Warnings.Num(), 3);

	// `fill` as an expression: built as a fill slot, its share read every frame.
	{
		FApexHudComponentDef Segments;
		FApexHudLoadReport SegmentsReport;
		TestTrue(TEXT("a fill expression parses"), ApexHud::ParseComponent(TEXT(R"({ "root": { "type": "row", "width": 400, "children": [
			{ "type": "bar", "fill": "=pit.service_tyres_share", "height": 6, "value": "=pit.service_tyres_fill" },
			{ "type": "bar", "fill": 2, "height": 6, "value": 0.5 }
		] } })"), TEXT("C:/hud/x"), Segments, SegmentsReport));
		TestEqual(TEXT("fill expression: no errors"), SegmentsReport.Errors.Num(), 0);
		TestEqual(TEXT("fill expression: no warnings"), SegmentsReport.Warnings.Num(), 0);
		if (Segments.Root.Children.Num() == 2)
		{
			TestTrue(TEXT("the share is dynamic"), Segments.Root.Children[0].FillShare.IsDynamic());
			TestEqual(TEXT("built as a fill slot"), Segments.Root.Children[0].Fill, 1.0f);
			TestFalse(TEXT("a number stays fixed"), Segments.Root.Children[1].FillShare.IsSet());
			TestEqual(TEXT("the number"), Segments.Root.Children[1].Fill, 2.0f);
		}
		TArray<FString> SegmentWarnings;
		ApexHud::CheckDataNames(Segments, SegmentWarnings);
		TestEqual(TEXT("the fill's names are checked and published"), SegmentWarnings.Num(), 0);
		TestTrue(TEXT("a fill that is neither"), Errors(TEXT(R"({ "root": { "type": "row", "children": [ { "type": "spacer", "fill": "half" } ] } })")).Errors.Num() > 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudComponentOverrideTest, "ApexSim.Hud.Component.Override", ApexTestFlags)

bool FApexHudComponentOverrideTest::RunTest(const FString& Parameters)
{
	const FString Dir = HudScratchDir();
	auto Write = [&Dir](const TCHAR* Relative, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Dir, Relative, TEXT("component.json")));
	};
	Write(TEXT("default/speed"), TEXT(R"({ "name": "Shipped", "root": { "type": "row" } })"));
	Write(TEXT("default/gear"), TEXT(R"({ "name": "Gear", "root": { "type": "row" } })"));
	Write(TEXT("default/broken"), TEXT(R"({ "root": { "type": "nope" } })"));
	Write(TEXT("custom/speed"), TEXT(R"({ "name": "Mine", "region": "center", "root": { "type": "row" } })"));
	Write(TEXT("custom/gear"), TEXT(R"({ "enabled": false })"));
	Write(TEXT("custom/extra"), TEXT(R"({ "name": "Extra", "root": { "type": "column" } })"));

	TArray<FApexHudComponentDef> Components;
	FApexHudLoadReport Report;
	ApexHud::LoadComponents({Dir}, Components, Report);

	const FApexHudComponentDef* Speed = Components.FindByPredicate([](const FApexHudComponentDef& C) { return C.Id == TEXT("speed"); });
	TestTrue(TEXT("custom replaces default"), Speed && Speed->Name == TEXT("Mine") && Speed->Region == TEXT("center"));
	TestFalse(TEXT("disabled component hidden"), Components.ContainsByPredicate([](const FApexHudComponentDef& C) { return C.Id == TEXT("gear"); }));
	TestTrue(TEXT("new component added"), Components.ContainsByPredicate([](const FApexHudComponentDef& C) { return C.Id == TEXT("extra"); }));
	TestFalse(TEXT("broken component left out"), Components.ContainsByPredicate([](const FApexHudComponentDef& C) { return C.Id == TEXT("broken"); }));
	TestEqual(TEXT("and reported"), Report.Errors.Num(), 1);
	TestEqual(TEXT("two components"), Components.Num(), 2);

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudDataBuildTest, "ApexSim.Hud.Data.Build", ApexTestFlags)

bool FApexHudDataBuildTest::RunTest(const FString& Parameters)
{
	constexpr float Length = 5000.0f;

	// Three cars on lap 2: Alice leads, the player (car 1) is second, Bob third.
	FApexTelemetryFrame Frame;
	Frame.Cars.Add(HudCar(0, 2, 1000.0f, 50.0f));
	FApexCarTelemetry Local = HudCar(1, 2, 800.0f, 40.0f);
	Local.EngineRpm = 9000.0f;
	Local.Gear = 4;
	Local.FuelLiters = 30.0f;
	Local.TyreTempC[0] = 60.0f;  // cold against 90 +- 10
	Local.TyreTempC[1] = 95.0f;  // ok
	Local.TyreTempC[2] = 105.0f; // hot
	Local.TyreTempC[3] = 130.0f; // over
	Local.DamagePct[0] = 0.0f;
	Local.DamagePct[1] = 0.0f;
	Local.DamagePct[2] = 0.0f;
	Local.DamagePct[3] = 0.0f;
	Local.DamagePct[4] = 0.0f;
	Frame.Cars.Add(Local);
	Frame.Cars.Add(HudCar(2, 2, 500.0f, 45.0f));

	FApexSessionRoster Roster;
	for (const TPair<int32, const TCHAR*>& Entry : {TPair<int32, const TCHAR*>(0, TEXT("Alice")), TPair<int32, const TCHAR*>(1, TEXT("Me")), TPair<int32, const TCHAR*>(2, TEXT("Bob"))})
	{
		FApexRosterEntry& Row = Roster.Entries.AddDefaulted_GetRef();
		Row.CarIndex = Entry.Key;
		Row.PlayerName = Entry.Value;
	}

	FApexTimingBoard Timing;
	Timing.SectorCount = 3;
	Timing.SessionBestSplitsMs = {30000, 31000, 0};
	Timing.SessionBestLapMs = 95000;
	Timing.SessionBestLapCarIndex = 0;
	FApexCarTiming& Mine = Timing.Cars.Add(1);
	Mine.CurrentSplitsMs = {29000, 32000, 0};
	Mine.BestSplitsMs = {29500, 31500, 0};

	FApexHudInputs In;
	In.Frame = &Frame;
	In.LocalCarIndex = 1;
	In.Roster = &Roster;
	In.Timing = &Timing;
	In.TrackLengthM = Length;
	In.LapLimit = 10;
	In.GameMode = EApexGameMode::Race;
	In.LimiterRpm = 12000.0f;

	FApexHudMemory Memory;
	FApexHudData Data;
	ApexHudData::Build(In, Memory, Data);

	TestEqual(TEXT("position"), HudValue(Data, TEXT("race.position")).AsNumber(), 2.0);
	TestEqual(TEXT("car count"), HudValue(Data, TEXT("race.car_count")).AsNumber(), 3.0);
	TestEqual(TEXT("ahead"), HudValue(Data, TEXT("gap.ahead_name")).AsString(), FString(TEXT("Alice")));
	TestEqual(TEXT("ahead by 200 m at 40 m/s"), HudValue(Data, TEXT("gap.ahead_s")).AsNumber(), 5.0, 1e-4);
	TestEqual(TEXT("behind"), HudValue(Data, TEXT("gap.behind_name")).AsString(), FString(TEXT("Bob")));
	TestEqual(TEXT("laps left"), HudValue(Data, TEXT("lap.laps_left")).AsNumber(), 9.0);
	TestEqual(TEXT("gear"), HudValue(Data, TEXT("car.gear_text")).AsString(), FString(TEXT("4")));
	TestEqual(TEXT("rpm against the limiter"), HudValue(Data, TEXT("car.rpm_fraction")).AsNumber(), 0.75, 1e-4);
	TestEqual(TEXT("fastest lap holder"), HudValue(Data, TEXT("timing.session_best_name")).AsString(), FString(TEXT("Alice")));
	TestTrue(TEXT("no delta without a reference"), HudValue(Data, TEXT("timing.delta_s")).IsNone());

	const TArray<FApexHudRecord>* Standings = Data.FindList(TEXT("standings"));
	TestTrue(TEXT("standings"), Standings && Standings->Num() == 3);
	if (Standings && Standings->Num() == 3)
	{
		TestEqual(TEXT("leader"), (*Standings)[0][TEXT("name")].AsString(), FString(TEXT("Alice")));
		TestTrue(TEXT("local second"), (*Standings)[1][TEXT("is_local")].AsBool());
		TestTrue(TEXT("leader has no gap"), (*Standings)[0][TEXT("gap_leader_s")].IsNone());
		TestEqual(TEXT("Bob to the leader"), (*Standings)[2][TEXT("gap_leader_s")].AsNumber(), 500.0 / 45.0, 1e-3);
	}
	TestFalse(TEXT("a lap race is not timed"), HudValue(Data, TEXT("race.timed")).AsBool());
	TestTrue(TEXT("a lap race has no clock"), HudValue(Data, TEXT("race.time_left_s")).IsNone());

	{
		// A timed race: no distance, the clock, laps to go from the car's own
		// last lap, then the leader's lap once the clock has run out.
		FApexTelemetryFrame Timed = Frame;
		Timed.RaceLeftMs = 600000;
		FApexCarTelemetry* Me = Timed.Cars.FindByPredicate([](const FApexCarTelemetry& Car) { return Car.CarIndex == 1; });
		if (TestNotNull(TEXT("the local car"), Me))
		{
			Me->LastLapTimeMs = 90000;
			Me->FinishPosition = 0;
			In.Frame = &Timed;
			In.LapLimit = 0;
			FApexHudMemory TimedMemory;
			FApexHudData TimedData;
			ApexHudData::Build(In, TimedMemory, TimedData);
			TestTrue(TEXT("timed"), HudValue(TimedData, TEXT("race.timed")).AsBool());
			TestEqual(TEXT("ten minutes left"), HudValue(TimedData, TEXT("race.time_left_s")).AsNumber(), 600.0);
			TestTrue(TEXT("no last lap yet"), HudValue(TimedData, TEXT("race.final_lap")).IsNone());
			TestEqual(TEXT("seven laps of 90 s"), HudValue(TimedData, TEXT("lap.laps_left")).AsNumber(), 7.0);
			TestFalse(TEXT("not the last lap"), HudValue(TimedData, TEXT("lap.final")).AsBool());

			Timed.RaceLeftMs = 0;
			Timed.RaceFinalLap = FMath::Max(1, Me->CurrentLap);
			ApexHudData::Build(In, TimedMemory, TimedData);
			TestEqual(TEXT("the leader's lap is the last"), HudValue(TimedData, TEXT("race.final_lap")).AsNumber(),
				static_cast<double>(Timed.RaceFinalLap));
			TestEqual(TEXT("one lap to go"), HudValue(TimedData, TEXT("lap.laps_left")).AsNumber(), 1.0);
			TestTrue(TEXT("the last lap"), HudValue(TimedData, TEXT("lap.final")).AsBool());
		}
		In.Frame = &Frame;
		In.LapLimit = 10;
	}

	const TArray<FApexHudRecord>* Sectors = Data.FindList(TEXT("sectors"));
	TestTrue(TEXT("three sectors"), Sectors && Sectors->Num() == 3);
	if (Sectors && Sectors->Num() == 3)
	{
		TestEqual(TEXT("under the session best"), (*Sectors)[0][TEXT("state")].AsString(), FString(TEXT("session_best")));
		TestEqual(TEXT("slower than my best"), (*Sectors)[1][TEXT("state")].AsString(), FString(TEXT("slower")));
		TestEqual(TEXT("not driven"), (*Sectors)[2][TEXT("state")].AsString(), FString(TEXT("none")));
	}

	TestEqual(TEXT("cold tyre"), HudValue(Data, TEXT("tyre.fl.state")).AsString(), FString(TEXT("cold")));
	TestEqual(TEXT("ok tyre"), HudValue(Data, TEXT("tyre.fr.state")).AsString(), FString(TEXT("ok")));
	TestEqual(TEXT("hot tyre"), HudValue(Data, TEXT("tyre.rl.state")).AsString(), FString(TEXT("hot")));
	TestEqual(TEXT("cooked tyre"), HudValue(Data, TEXT("tyre.rr.state")).AsString(), FString(TEXT("over")));

	// A fresh hit flashes its zone, fading over 0.8 s.
	Frame.Cars[1].DamagePct[0] = 12.0f;
	In.TimeSeconds = 10.0;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("hit flashes"), HudValue(Data, TEXT("damage.front_flash")).AsNumber(), 1.0, 1e-4);
	In.TimeSeconds = 10.4;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("flash fades"), HudValue(Data, TEXT("damage.front_flash")).AsNumber(), 0.5, 1e-3);
	TestEqual(TEXT("rear untouched"), HudValue(Data, TEXT("damage.rear_flash")).AsNumber(), 0.0);

	// A lap of fuel is measured at the line; 4 L a lap with 8 laps to go is short.
	Frame.Cars[1].CurrentLap = 3;
	Frame.Cars[1].FuelLiters = 26.0f;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("per lap"), HudValue(Data, TEXT("fuel.per_lap")).AsNumber(), 4.0, 1e-4);
	TestEqual(TEXT("short of the flag"), HudValue(Data, TEXT("fuel.state")).AsString(), FString(TEXT("short")));

	// The delta: a legal lap sampled into the memory becomes the reference.
	FApexHudMemory Delta;
	FApexCarTelemetry Lap = HudCar(1, 1, 0.0f, 50.0f);
	for (int32 Step = 0; Step <= 10; ++Step)
	{
		Lap.TrackProgress = Length * Step / 10.0f;
		Lap.CurrentLapTimeMs = Step * 10000;
		Delta.SampleLap(Lap, Length);
	}
	Lap.CurrentLap = 2;
	Lap.LastLapTimeMs = 100000;
	Lap.TrackProgress = 0.0f;
	Lap.CurrentLapTimeMs = 0;
	Delta.SampleLap(Lap, Length);
	TestEqual(TEXT("reference lap"), Delta.ReferenceLapSeconds, 100.0f);
	TestEqual(TEXT("halfway on the reference"), Delta.ReferenceTimeAt(0.5f), 50.0f, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudDataWatchingTest, "ApexSim.Hud.Data.Watching", ApexTestFlags)

bool FApexHudDataWatchingTest::RunTest(const FString& Parameters)
{
	constexpr float Length = 4000.0f;

	// Watching car 1, second on the road; car 2 is a lap down on softs, car 3 is out.
	FApexTelemetryFrame Frame;
	FApexCarTelemetry Leader = HudCar(0, 5, 2000.0f, 50.0f);
	Leader.Compound = 2;
	Frame.Cars.Add(Leader);
	FApexCarTelemetry Watched = HudCar(1, 5, 1500.0f, 50.0f);
	Watched.Compound = 1;
	Watched.TyreWearPct[0] = 12.0f;
	Watched.TyreWearPct[1] = 31.0f;
	Watched.TyreWearPct[2] = 9.0f;
	Watched.TyreWearPct[3] = 10.0f;
	Frame.Cars.Add(Watched);
	FApexCarTelemetry Lapped = HudCar(2, 4, 1700.0f, 40.0f);
	Lapped.Compound = 0;
	Frame.Cars.Add(Lapped);
	FApexCarTelemetry Out = HudCar(3, 3, 100.0f, 0.0f);
	for (float& Pct : Out.DamagePct)
	{
		Pct = 0.0f;
	}
	Out.DamagePct[4] = 100.0f;
	Frame.Cars.Add(Out);

	FApexSessionRoster Roster;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FApexRosterEntry& Row = Roster.Entries.AddDefaulted_GetRef();
		Row.CarIndex = Index;
		Row.PlayerName = FString::Printf(TEXT("Driver %d"), Index);
	}
	TMap<int32, FString> CarNames = {{0, TEXT("Posh GT")}, {1, TEXT("Zomba")}};

	FApexHudInputs In;
	In.Frame = &Frame;
	In.LocalCarIndex = 1;
	In.Roster = &Roster;
	In.TrackLengthM = Length;
	In.LapLimit = 12;
	In.GameMode = EApexGameMode::Race;
	In.bSpectating = true;
	In.SpectateSource = TEXT("showcase");
	In.SpectateCamera = TEXT("TV");
	In.bSpectateAuto = true;
	In.SpectateTowerMode = TEXT("tyres");
	In.CarNames = &CarNames;

	FApexHudMemory Memory;
	FApexHudData Data;
	ApexHudData::Build(In, Memory, Data);

	TestTrue(TEXT("watching"), HudValue(Data, TEXT("spectate.active")).AsBool());
	TestFalse(TEXT("a recorded race"), HudValue(Data, TEXT("spectate.live")).AsBool());
	TestEqual(TEXT("source"), HudValue(Data, TEXT("spectate.source")).AsString(), FString(TEXT("showcase")));
	TestEqual(TEXT("camera"), HudValue(Data, TEXT("spectate.camera")).AsString(), FString(TEXT("TV")));
	TestEqual(TEXT("tower"), HudValue(Data, TEXT("spectate.tower_mode")).AsString(), FString(TEXT("tyres")));
	TestEqual(TEXT("leader's lap"), HudValue(Data, TEXT("race.leader_lap")).AsNumber(), 5.0);
	TestEqual(TEXT("the watched car's driver"), HudValue(Data, TEXT("car.driver_name")).AsString(), FString(TEXT("Driver 1")));
	TestEqual(TEXT("its place"), HudValue(Data, TEXT("race.position")).AsNumber(), 2.0);
	TestEqual(TEXT("its compound"), HudValue(Data, TEXT("tyre.compound")).AsString(), FString(TEXT("M")));
	TestEqual(TEXT("its most worn tyre"), HudValue(Data, TEXT("tyre.wear_max_pct")).AsNumber(), 31.0, 1e-4);
	TestEqual(TEXT("on its first set since lap 1"), HudValue(Data, TEXT("tyre.age_laps")).AsNumber(), 4.0);

	const TArray<FApexHudRecord>* Standings = Data.FindList(TEXT("standings"));
	TestTrue(TEXT("standings"), Standings && Standings->Num() == 4);
	if (!Standings || Standings->Num() != 4)
	{
		return false;
	}
	const FApexHudRecord& First = (*Standings)[0];
	const FApexHudRecord& Second = (*Standings)[1];
	const FApexHudRecord& Third = (*Standings)[2];
	const FApexHudRecord& Fourth = (*Standings)[3];
	TestTrue(TEXT("the watched car is the HUD's"), Second[TEXT("is_local")].AsBool());
	TestFalse(TEXT("but it is not the player's"), Second[TEXT("is_player")].AsBool());
	TestEqual(TEXT("its model"), Second[TEXT("car_name")].AsString(), FString(TEXT("Zomba")));
	TestTrue(TEXT("a car this machine lacks has no model"), Third[TEXT("car_name")].IsNone());
	TestEqual(TEXT("interval: 500 m at 50 m/s"), Second[TEXT("interval_s")].AsNumber(), 10.0, 1e-4);
	TestTrue(TEXT("the leader has no interval"), First[TEXT("interval_s")].IsNone());
	TestEqual(TEXT("the lapped car is a lap down"), Third[TEXT("laps_down")].AsNumber(), 1.0);
	TestEqual(TEXT("its interval is to the car one place ahead: 3 800 m at 40 m/s"), Third[TEXT("interval_s")].AsNumber(), 95.0, 1e-3);
	TestEqual(TEXT("softs"), Third[TEXT("compound")].AsString(), FString(TEXT("S")));
	TestTrue(TEXT("the car out of the race"), Fourth[TEXT("retired")].AsBool());
	TestTrue(TEXT("wear unknown is null"), Third[TEXT("tyre_wear_pct")].IsNone());

	// A stop: the car comes to rest in its box, is serviced, and goes out on new tyres.
	Frame.Cars[1].bInPitLane = true;
	Frame.Cars[1].bPitServicing = true;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("a stop counted at the box"), HudValue(Data, TEXT("car.pit_stops")).AsNumber(), 1.0);
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("once"), HudValue(Data, TEXT("car.pit_stops")).AsNumber(), 1.0);
	Frame.Cars[1].bPitServicing = false;
	Frame.Cars[1].Compound = 0;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("fresh softs"), HudValue(Data, TEXT("tyre.age_laps")).AsNumber(), 0.0);
	Frame.Cars[1].CurrentLap = 7;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("two laps on them"), HudValue(Data, TEXT("tyre.age_laps")).AsNumber(), 2.0);

	// Watching another car keeps the field's history but not the delta.
	Memory.ReferenceLapSeconds = 90.0f;
	Memory.ResetForNewCar();
	TestEqual(TEXT("the reference goes"), Memory.ReferenceLapSeconds, 0.0f);
	TestTrue(TEXT("the stops stay"), Memory.Cars.Contains(1) && Memory.Cars[1].PitStops == 1);

	// The player's own race: the player's row says so.
	In.bSpectating = false;
	ApexHudData::Build(In, Memory, Data);
	TestFalse(TEXT("not watching"), HudValue(Data, TEXT("spectate.active")).AsBool());
	TestTrue(TEXT("no source"), HudValue(Data, TEXT("spectate.source")).IsNone());
	const TArray<FApexHudRecord>* Driving = Data.FindList(TEXT("standings"));
	// Car 1 is on lap 7 by now, and leads.
	const FApexHudRecord* Mine = Driving ? Driving->FindByPredicate(
		[](const FApexHudRecord& Row) { return Row[TEXT("car_index")].AsNumber() == 1.0; }) : nullptr;
	TestTrue(TEXT("the player's row"), Mine && (*Mine)[TEXT("is_player")].AsBool());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudDataStandingsHoldTest, "ApexSim.Hud.Data.StandingsHold", ApexTestFlags)

bool FApexHudDataStandingsHoldTest::RunTest(const FString& Parameters)
{
	// Two cars side by side, trading the lead on the road every frame: the
	// places and gaps move at most twice a second.
	FApexTelemetryFrame Frame;
	Frame.Cars.Add(HudCar(0, 3, 1001.0f, 50.0f));
	Frame.Cars.Add(HudCar(1, 3, 1000.0f, 50.0f));
	Frame.Cars.Add(HudCar(2, 3, 900.0f, 50.0f));

	FApexHudInputs In;
	In.Frame = &Frame;
	In.LocalCarIndex = 1;
	In.TrackLengthM = 4000.0f;
	In.LapLimit = 10;
	In.GameMode = EApexGameMode::Race;

	FApexHudMemory Memory;
	FApexHudData Data;
	In.TimeSeconds = 10.0;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("second"), HudValue(Data, TEXT("race.position")).AsNumber(), 2.0);
	TestEqual(TEXT("a metre behind at 50 m/s"), HudValue(Data, TEXT("gap.ahead_s")).AsNumber(), 0.02, 1e-4);

	Frame.Cars[1].TrackProgress = 1003.0f;
	Frame.Cars[2].TrackProgress = 950.0f;
	In.TimeSeconds = 10.2;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("still second a moment later"), HudValue(Data, TEXT("race.position")).AsNumber(), 2.0);
	TestEqual(TEXT("the gap held too"), HudValue(Data, TEXT("gap.ahead_s")).AsNumber(), 0.02, 1e-4);
	const TArray<FApexHudRecord>* Held = Data.FindList(TEXT("standings"));
	TestTrue(TEXT("the tower held"), Held && Held->Num() == 3 && (*Held)[0][TEXT("car_index")].AsNumber() == 0.0);

	In.TimeSeconds = 10.5;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("leads half a second on"), HudValue(Data, TEXT("race.position")).AsNumber(), 1.0);
	TestEqual(TEXT("the car behind by its new gap"), HudValue(Data, TEXT("gap.behind_s")).AsNumber(), 0.04, 1e-4);

	// A car leaving the field takes the order at once.
	Frame.Cars.RemoveAt(0);
	In.TimeSeconds = 10.6;
	ApexHudData::Build(In, Memory, Data);
	const TArray<FApexHudRecord>* Fewer = Data.FindList(TEXT("standings"));
	TestTrue(TEXT("two cars"), Fewer && Fewer->Num() == 2);

	// A replay seek runs the clock back: taken at once.
	Frame.Cars[1].TrackProgress = 1100.0f;
	In.TimeSeconds = 4.0;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("passed after the seek"), HudValue(Data, TEXT("race.position")).AsNumber(), 2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudPitProgressTest, "ApexSim.Hud.Pit.Progress", ApexTestFlags)

bool FApexHudPitProgressTest::RunTest(const FString& Parameters)
{
	auto Near = [this](const TCHAR* What, float Actual, float Expected)
	{
		TestTrue(*FString::Printf(TEXT("%s: %.3f, expected %.3f"), What, Actual, Expected), FMath::IsNearlyEqual(Actual, Expected, 0.001f));
	};

	// Tyres alone: one part, the whole bar.
	{
		FApexPitService Stop;
		Stop.TyresS = 2.5f;
		Stop.Compound = 2;
		Stop.TotalS = 2.5f;
		const FApexPitStopProgress P = ApexHudData::PitStopProgress(Stop, 1.5f);
		TestTrue(TEXT("tyres only: valid"), P.bValid);
		TestEqual(TEXT("tyres only: phase"), P.Phase, static_cast<int32>(FApexPitStopProgress::Tyres));
		TestEqual(TEXT("tyres only: key"), P.PhaseKey, FString(TEXT("tyres")));
		TestEqual(TEXT("tyres only: label"), P.PhaseLabel, FString(TEXT("CHANGING TYRES · HARD")));
		Near(TEXT("tyres only: elapsed"), P.ElapsedS, 1.0f);
		Near(TEXT("tyres only: share"), P.PartShare[FApexPitStopProgress::Tyres], 1.0f);
		Near(TEXT("tyres only: fill"), P.PartFill[FApexPitStopProgress::Tyres], 0.4f);
		Near(TEXT("tyres only: phase left"), P.PhaseLeftS, 1.5f);
		Near(TEXT("tyres only: no fuel share"), P.PartShare[FApexPitStopProgress::Fuel], 0.0f);
		Near(TEXT("tyres only: no repair share"), P.PartShare[FApexPitStopProgress::Repair], 0.0f);
	}

	// All three parts: 9 s of softs, 12.5 s for 25 L, 2 s of repairs.
	FApexPitService Full;
	Full.TyresS = 9.0f;
	Full.Compound = 0;
	Full.FuelS = 12.5f;
	Full.FuelL = 25.0f;
	Full.RepairS = 2.0f;
	Full.RepairPct = 50.0f;
	Full.TotalS = 23.5f;
	{
		// Just stopped.
		const FApexPitStopProgress P = ApexHudData::PitStopProgress(Full, 23.5f);
		TestEqual(TEXT("start: tyres"), P.Phase, static_cast<int32>(FApexPitStopProgress::Tyres));
		TestEqual(TEXT("start: label"), P.PhaseLabel, FString(TEXT("CHANGING TYRES · SOFT")));
		Near(TEXT("start: progress"), P.Progress, 0.0f);
		Near(TEXT("start: tyre share"), P.PartShare[FApexPitStopProgress::Tyres], 9.0f / 23.5f);
		Near(TEXT("start: fuel share"), P.PartShare[FApexPitStopProgress::Fuel], 12.5f / 23.5f);
		Near(TEXT("start: repair share"), P.PartShare[FApexPitStopProgress::Repair], 2.0f / 23.5f);
	}
	{
		// Twelve seconds in: the tyres on, three seconds of fuel gone.
		const FApexPitStopProgress P = ApexHudData::PitStopProgress(Full, 11.5f);
		TestEqual(TEXT("12 s: fuel"), P.Phase, static_cast<int32>(FApexPitStopProgress::Fuel));
		TestEqual(TEXT("12 s: key"), P.PhaseKey, FString(TEXT("fuel")));
		TestEqual(TEXT("12 s: label"), P.PhaseLabel, FString(TEXT("REFUELLING · +25.0 L")));
		Near(TEXT("12 s: elapsed"), P.ElapsedS, 12.0f);
		Near(TEXT("12 s: tyres full"), P.PartFill[FApexPitStopProgress::Tyres], 1.0f);
		Near(TEXT("12 s: fuel fill"), P.PartFill[FApexPitStopProgress::Fuel], 3.0f / 12.5f);
		Near(TEXT("12 s: repairs not begun"), P.PartFill[FApexPitStopProgress::Repair], 0.0f);
		Near(TEXT("12 s: phase left"), P.PhaseLeftS, 9.5f);
		Near(TEXT("12 s: phase progress"), P.PhaseProgress, 3.0f / 12.5f);
	}
	{
		// At the boundary the next part has begun.
		const FApexPitStopProgress P = ApexHudData::PitStopProgress(Full, 2.0f);
		TestEqual(TEXT("21.5 s: repairs"), P.Phase, static_cast<int32>(FApexPitStopProgress::Repair));
		TestEqual(TEXT("21.5 s: label"), P.PhaseLabel, FString(TEXT("REPAIRING · 50%")));
		Near(TEXT("21.5 s: fuel full"), P.PartFill[FApexPitStopProgress::Fuel], 1.0f);
	}
	{
		// The last tick of the stop: the last part, full.
		const FApexPitStopProgress P = ApexHudData::PitStopProgress(Full, 0.0f);
		TestEqual(TEXT("done: repairs"), P.Phase, static_cast<int32>(FApexPitStopProgress::Repair));
		Near(TEXT("done: progress"), P.Progress, 1.0f);
		Near(TEXT("done: phase progress"), P.PhaseProgress, 1.0f);
		Near(TEXT("done: phase left"), P.PhaseLeftS, 0.0f);
	}

	// A part the stop does not do is skipped: tyres and repairs, no fuel.
	{
		FApexPitService Stop;
		Stop.TyresS = 9.0f;
		Stop.Compound = 1;
		Stop.RepairS = 4.0f;
		Stop.RepairPct = 20.0f;
		Stop.TotalS = 13.0f;
		const FApexPitStopProgress P = ApexHudData::PitStopProgress(Stop, 4.0f);
		TestEqual(TEXT("no fuel: straight to repairs"), P.Phase, static_cast<int32>(FApexPitStopProgress::Repair));
		Near(TEXT("no fuel: no share"), P.PartShare[FApexPitStopProgress::Fuel], 0.0f);
		Near(TEXT("no fuel: no fill"), P.PartFill[FApexPitStopProgress::Fuel], 0.0f);
		Near(TEXT("no fuel: repair share"), P.PartShare[FApexPitStopProgress::Repair], 4.0f / 13.0f);
		Near(TEXT("no fuel: repair fill"), P.PartFill[FApexPitStopProgress::Repair], 0.0f);
	}

	// An empty plan is no stop.
	TestFalse(TEXT("empty plan"), ApexHudData::PitStopProgress(FApexPitService(), 0.0f).bValid);

	// Through the data points: the player's car stopped twelve seconds in.
	FApexTelemetryFrame Frame;
	FApexCarTelemetry Car = HudCar(2, 3, 100.0f, 0.0f);
	Car.bInPitLane = true;
	Car.bPitServicing = true;
	Car.ServiceSecondsLeft = 11.5f;
	Frame.Cars.Add(Car);
	TMap<int32, FApexPitService> Stops;
	Full.CarIndex = 2;
	Full.PitBox = 6;
	Stops.Add(2, Full);
	FApexHudInputs In;
	In.Frame = &Frame;
	In.LocalCarIndex = 2;
	In.PitServices = &Stops;
	FApexHudMemory Memory;
	FApexHudData Data;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("box, from 1"), HudValue(Data, TEXT("pit.box")).AsNumber(), 7.0);
	TestEqual(TEXT("phase"), HudValue(Data, TEXT("pit.service_phase")).AsString(), FString(TEXT("fuel")));
	TestEqual(TEXT("compound"), HudValue(Data, TEXT("pit.service_compound")).AsString(), FString(TEXT("SOFT")));
	TestEqual(TEXT("litres"), HudValue(Data, TEXT("pit.service_fuel_l")).AsNumber(), 25.0);
	TestEqual(TEXT("total"), HudValue(Data, TEXT("pit.service_total_s")).AsNumber(), 23.5);
	TestFalse(TEXT("not on autopilot"), HudValue(Data, TEXT("pit.autopilot")).AsBool());

	// Rolling out of the box on the autopilot: the plan stays, the progress goes.
	Frame.Cars[0].bPitServicing = false;
	Frame.Cars[0].bPitAutopilot = true;
	ApexHudData::Build(In, Memory, Data);
	TestTrue(TEXT("autopilot"), HudValue(Data, TEXT("pit.autopilot")).AsBool());
	TestEqual(TEXT("still box 7"), HudValue(Data, TEXT("pit.box")).AsNumber(), 7.0);
	TestTrue(TEXT("no phase off the box"), HudValue(Data, TEXT("pit.service_phase")).IsNone());
	TestTrue(TEXT("no progress off the box"), HudValue(Data, TEXT("pit.service_progress")).IsNone());

	// Held at the red light.
	Frame.Cars[0].bPitAutopilot = false;
	Frame.Cars[0].bPitHeld = true;
	Frame.Cars[0].bPitExitClosed = true;
	ApexHudData::Build(In, Memory, Data);
	TestTrue(TEXT("held"), HudValue(Data, TEXT("pit.held")).AsBool());
	TestTrue(TEXT("exit closed"), HudValue(Data, TEXT("pit.exit_closed")).AsBool());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudDataStableTest, "ApexSim.Hud.Data.Stable", ApexTestFlags)

bool FApexHudDataStableTest::RunTest(const FString& Parameters)
{
	// Every name exists whatever the game state, so a component never sees a
	// name come and go, and the empty build is the whole catalogue.
	FApexTelemetryFrame Frame;
	FApexCarTelemetry Car = HudCar(0, 1, 100.0f, 30.0f);
	Car.FuelLiters = 20.0f;
	Car.ErsChargePct = 50.0f;
	Car.TyreTempC[0] = Car.TyreTempC[1] = Car.TyreTempC[2] = Car.TyreTempC[3] = 90.0f;
	Car.DamagePct[0] = Car.DamagePct[1] = Car.DamagePct[2] = Car.DamagePct[3] = Car.DamagePct[4] = 0.0f;
	Frame.Cars.Add(Car);
	FApexHudInputs In;
	In.Frame = &Frame;
	In.LocalCarIndex = 0;
	In.bHasConditions = true;
	In.PingMs = 30;
	FApexHudMemory Memory;
	FApexHudData Full;
	ApexHudData::Build(In, Memory, Full);
	TArray<FName> FullNames;
	Full.Values.GetKeys(FullNames);
	FullNames.Sort(FNameLexicalLess());

	const TArray<FName> EmptyNames = ApexHudData::ScalarNames();
	for (const FName& Name : FullNames)
	{
		TestTrue(*FString::Printf(TEXT("%s also without a car"), *Name.ToString()), EmptyNames.Contains(Name));
	}
	for (const FName& Name : EmptyNames)
	{
		TestTrue(*FString::Printf(TEXT("%s also with a car"), *Name.ToString()), FullNames.Contains(Name));
	}

	// Every list record carries exactly its declared fields.
	for (const TPair<FName, TArray<FName>>& List : ApexHudData::ListFields())
	{
		const TArray<FApexHudRecord>* Rows = Full.FindList(List.Key);
		TestTrue(*FString::Printf(TEXT("%s filled"), *List.Key.ToString()), Rows && Rows->Num() > 0);
		if (Rows && Rows->Num() > 0)
		{
			TArray<FName> Fields;
			(*Rows)[0].GetKeys(Fields);
			TestEqual(*FString::Printf(TEXT("%s fields"), *List.Key.ToString()), Fields.Num(), List.Value.Num());
			for (const FName& Field : List.Value)
			{
				TestTrue(*FString::Printf(TEXT("%s.%s"), *List.Key.ToString(), *Field.ToString()), (*Rows)[0].Contains(Field));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudShippedTest, "ApexSim.Hud.Shipped", ApexTestFlags)

bool FApexHudShippedTest::RunTest(const FString& Parameters)
{
	// Every shipped component loads clean: no errors, and no warnings either,
	// since a warning is almost always a misspelt data point.
	const FString Dir = HudRepoPath(TEXT("content/hud"));
	TArray<FApexHudComponentDef> Components;
	FApexHudLoadReport Report;
	ApexHud::LoadComponents({Dir}, Components, Report);
	for (const FString& Error : Report.Errors)
	{
		AddError(Error);
	}
	for (const FString& Warning : Report.Warnings)
	{
		AddError(Warning);
	}
	TestTrue(TEXT("the shipped HUD has components"), Components.Num() >= 10);
	for (const TCHAR* Id : {TEXT("track_info"), TEXT("race_state"), TEXT("status"), TEXT("minimap"), TEXT("standings"),
			 TEXT("timing"), TEXT("pedals"), TEXT("damage"), TEXT("car_state"), TEXT("mirror")})
	{
		const FApexHudComponentDef* Found = Components.FindByPredicate([Id](const FApexHudComponentDef& C) { return C.Id == Id; });
		TestTrue(*FString::Printf(TEXT("has %s, shown"), Id), Found && Found->bDefaultEnabled);
	}
	// The extras the HUD editor offers to add.
	for (const TCHAR* Id : {TEXT("relative"), TEXT("speed_gear"), TEXT("conditions")})
	{
		const FApexHudComponentDef* Found = Components.FindByPredicate([Id](const FApexHudComponentDef& C) { return C.Id == Id; });
		TestTrue(*FString::Printf(TEXT("has %s, off until added"), Id), Found && !Found->bDefaultEnabled);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudDocumentedTest, "ApexSim.Hud.Data.Documented", ApexTestFlags)

bool FApexHudDocumentedTest::RunTest(const FString& Parameters)
{
	// The data points are the modding API: each one is in the reference.
	FString Doc;
	if (!FFileHelper::LoadFileToString(Doc, *HudRepoPath(TEXT("docs/HUD_MODDING.md"))))
	{
		AddError(TEXT("docs/HUD_MODDING.md is missing"));
		return false;
	}
	auto Documented = [&Doc](const FString& Name) { return Doc.Contains(TEXT("`") + Name + TEXT("`")); };

	for (const FName& Name : ApexHudData::ScalarNames())
	{
		const FString Text = Name.ToString();
		// The per-tyre and per-zone scalars are documented once, as a pattern.
		if (Text.StartsWith(TEXT("tyre.f")) || Text.StartsWith(TEXT("tyre.r")))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("`%s` documented"), *Text), Documented(Text));
	}
	for (const TPair<FName, TArray<FName>>& List : ApexHudData::ListFields())
	{
		TestTrue(*FString::Printf(TEXT("list `%s` documented"), *List.Key.ToString()), Documented(List.Key.ToString()));
		for (const FName& Field : List.Value)
		{
			TestTrue(*FString::Printf(TEXT("`%s.%s` documented"), *List.Key.ToString(), *Field.ToString()),
				Documented(FString::Printf(TEXT("item.%s"), *Field.ToString())) || Documented(Field.ToString()));
		}
	}
	for (const FString& Function : FApexHudExpr::FunctionNames())
	{
		// Written as a call in the table: `fmt_time(s)`.
		TestTrue(*FString::Printf(TEXT("function `%s` documented"), *Function), Doc.Contains(TEXT("`") + Function + TEXT("(")));
	}
	TestTrue(TEXT("tyre pattern documented"), Documented(TEXT("tyre.<fl|fr|rl|rr>.<field>")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudLayoutJsonTest, "ApexSim.Hud.Layout.Json", ApexTestFlags)

bool FApexHudLayoutJsonTest::RunTest(const FString& Parameters)
{
	FApexHudLayout Layout;
	FApexHudPlacement Pinned;
	Pinned.bPinned = true;
	Pinned.Anchor = FVector2D(1.0, 1.0);
	Pinned.Position = FVector2D(-56.0, -30.0);
	Pinned.Scale = 1.25f;
	Layout.Components.Add(TEXT("car_state"), Pinned);
	FApexHudPlacement Hidden;
	Hidden.bEnabled = false;
	Layout.Components.Add(TEXT("minimap"), Hidden);
	FApexHudPlacement Added;
	Layout.Components.Add(TEXT("relative"), Added);

	FApexHudLayout Back;
	FString Error;
	TestTrue(TEXT("reads what it writes"), Back.FromJson(Layout.ToJson(), Error));
	TestTrue(TEXT("no complaint"), Error.IsEmpty());
	TestEqual(TEXT("three entries"), Back.Components.Num(), 3);
	TestTrue(TEXT("pinned survives"), Back.Find(TEXT("car_state")) && *Back.Find(TEXT("car_state")) == Pinned);
	TestTrue(TEXT("hidden survives"), Back.Find(TEXT("minimap")) && !Back.Find(TEXT("minimap"))->bEnabled);
	TestTrue(TEXT("added survives"), Back.Find(TEXT("relative")) && Back.Find(TEXT("relative"))->bEnabled && !Back.Find(TEXT("relative"))->bPinned);

	FApexHudLayout OnlyAdded;
	OnlyAdded.Components.Add(TEXT("relative"), Added);
	TestFalse(TEXT("an unpinned entry writes no position"), OnlyAdded.ToJson().Contains(TEXT("\"position\"")));

	TestTrue(TEXT("enabled from the layout"), Back.IsEnabled(TEXT("relative"), false));
	TestFalse(TEXT("hidden by the layout"), Back.IsEnabled(TEXT("minimap"), true));
	TestTrue(TEXT("else the component's own default"), Back.IsEnabled(TEXT("standings"), true));
	TestEqual(TEXT("scale"), Back.ScaleOf(TEXT("car_state")), 1.25f);
	TestEqual(TEXT("default scale"), Back.ScaleOf(TEXT("standings")), 1.0f);

	// Written by hand: comments, trailing commas, a scale out of range, half a pin.
	FApexHudLayout Hand;
	TestTrue(TEXT("hand-written file"), Hand.FromJson(TEXT(R"({
		// mine
		"components": {
			"standings": { "scale": 9, },
			"timing": { "anchor": [0, 1] },
		},
	})"), Error));
	TestEqual(TEXT("scale held to the range"), Hand.ScaleOf(TEXT("standings")), FApexHudLayout::MaxScale);
	TestFalse(TEXT("half a pin is no pin"), Hand.Find(TEXT("timing")) && Hand.Find(TEXT("timing"))->bPinned);
	TestTrue(TEXT("and is reported"), Error.Contains(TEXT("timing")));
	TestFalse(TEXT("not JSON"), Hand.FromJson(TEXT("{ nope"), Error));

	// To disk and back; a missing file is an empty layout.
	const FString Dir = HudScratchDir();
	const FString File = FPaths::Combine(Dir, TEXT("custom"), TEXT("layout.json"));
	TestTrue(TEXT("saves"), Layout.Save(File));
	FApexHudLayout Loaded;
	TestTrue(TEXT("loads"), Loaded.Load(File, Error) && Loaded.Components.Num() == 3);
	TestTrue(TEXT("missing file is fine"), Loaded.Load(FPaths::Combine(Dir, TEXT("none.json")), Error) && Loaded.Components.IsEmpty());
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudLayoutPinTest, "ApexSim.Hud.Layout.Pin", ApexTestFlags)

bool FApexHudLayoutPinTest::RunTest(const FString& Parameters)
{
	const FVector2D Screen(1920.0, 1080.0);

	// The car panel in the bottom-right corner keeps to that corner.
	const FSlateRect Corner(1394.0f, 820.0f, 1864.0f, 1050.0f);
	const FApexHudPlacement BottomRight = ApexHudPlace::PinRect(Corner, Screen, 1.0f);
	TestTrue(TEXT("anchored bottom right"), HudSame(BottomRight.Anchor, FVector2D(1.0, 1.0)));
	TestTrue(TEXT("56 in, 30 up"), HudSame(BottomRight.Position, FVector2D(-56.0, -30.0)));
	TestTrue(TEXT("nothing moves"), HudSame(FVector2D(ApexHudPlace::TopLeft(BottomRight, Corner.GetSize(), Screen)), FVector2D(Corner.GetTopLeft())));
	// On a wider, taller screen it is still 56 and 30 from that corner.
	const FVector2D Wide(2560.0, 1440.0);
	const FVector2D WideTopLeft = ApexHudPlace::TopLeft(BottomRight, Corner.GetSize(), Wide);
	TestTrue(TEXT("follows the corner"), HudSame(WideTopLeft + FVector2D(Corner.GetSize()), Wide - FVector2D(56.0, 30.0)));

	// The middle third is the middle.
	const FSlateRect Middle(860.0f, 500.0f, 1060.0f, 560.0f);
	const FApexHudPlacement Centre = ApexHudPlace::PinRect(Middle, Screen, 1.5f);
	TestTrue(TEXT("anchored in the middle"), HudSame(Centre.Anchor, FVector2D(0.5, 0.5)));
	TestEqual(TEXT("scale kept"), Centre.Scale, 1.5f);
	TestTrue(TEXT("middle stays put"), HudSame(FVector2D(ApexHudPlace::TopLeft(Centre, Middle.GetSize(), Screen)), FVector2D(Middle.GetTopLeft())));

	// Top left, by the top-left gutters.
	const FApexHudPlacement TopLeft = ApexHudPlace::PinRect(FSlateRect(56.0f, 30.0f, 400.0f, 120.0f), Screen, 1.0f);
	TestTrue(TEXT("anchored top left"), HudSame(TopLeft.Anchor, FVector2D(0.0, 0.0)));
	TestTrue(TEXT("position is the corner"), HudSame(TopLeft.Position, FVector2D(56.0, 30.0)));

	TestEqual(TEXT("scale floor"), ApexHudPlace::ClampScale(0.1f), FApexHudLayout::MinScale);
	TestEqual(TEXT("scale ceiling"), ApexHudPlace::ClampScale(3.0f), FApexHudLayout::MaxScale);
	TestEqual(TEXT("5% steps"), ApexHudPlace::ClampScale(1.234f), 1.25f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHudLayoutSnapTest, "ApexSim.Hud.Layout.Snap", ApexTestFlags)

bool FApexHudLayoutSnapTest::RunTest(const FString& Parameters)
{
	const FVector2D Screen(1920.0, 1080.0);
	ApexHudPlace::FSnapLines Lines;

	// 5 off the left gutter and 3 off the bottom one: both snap.
	const FSlateRect Near(61.0f, 900.0f, 361.0f, 1047.0f);
	const FSlateRect Snapped = ApexHudPlace::Snap(Near, Screen, {}, 8.0f, Lines);
	TestEqual(TEXT("left edge on the gutter"), Snapped.Left, 56.0f);
	TestEqual(TEXT("bottom edge on the gutter"), Snapped.Bottom, 1050.0f);
	TestTrue(TEXT("both lines reported"), Lines.X.IsSet() && Lines.Y.IsSet());
	TestTrue(TEXT("size kept"), HudSame(FVector2D(Snapped.GetSize()), FVector2D(Near.GetSize())));

	// Beside another panel: its right edge meets the other's left.
	const FSlateRect Other(600.0f, 700.0f, 900.0f, 1050.0f);
	const FSlateRect Beside(305.0f, 300.0f, 596.0f, 400.0f);
	const FSlateRect Joined = ApexHudPlace::Snap(Beside, Screen, {Other}, 8.0f, Lines);
	TestEqual(TEXT("edge to edge"), Joined.Right, 600.0f);

	// The screen's centre line catches a centred panel.
	const FSlateRect Centred(856.0f, 200.0f, 1056.0f, 260.0f);
	TestEqual(TEXT("centred"), static_cast<double>(ApexHudPlace::Snap(Centred, Screen, {}, 8.0f, Lines).GetCenter().X), 960.0);

	// Too far from anything: left alone.
	const FSlateRect Free(300.0f, 300.0f, 500.0f, 350.0f);
	const FSlateRect Same = ApexHudPlace::Snap(Free, Screen, {}, 8.0f, Lines);
	TestTrue(TEXT("not moved"), HudSame(FVector2D(Same.GetTopLeft()), FVector2D(Free.GetTopLeft())));
	TestFalse(TEXT("no lines"), Lines.X.IsSet() || Lines.Y.IsSet());
	return true;
}

#endif
