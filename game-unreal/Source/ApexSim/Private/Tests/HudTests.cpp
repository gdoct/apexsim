#include "ApexTestCommon.h"
#include "HAL/FileManager.h"
#include "Hud/ApexHudComponent.h"
#include "Hud/ApexHudData.h"
#include "Hud/ApexHudExpression.h"
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
		TestTrue(*FString::Printf(TEXT("has %s"), Id), Components.ContainsByPredicate([Id](const FApexHudComponentDef& C) { return C.Id == Id; }));
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

#endif
