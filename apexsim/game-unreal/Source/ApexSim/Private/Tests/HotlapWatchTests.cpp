#include "ApexTestCommon.h"
#include "ApexRepoPaths.h"
#include "ApexNetSubsystem.h"
#include "Engine/GameInstance.h"
#include "Hud/ApexHudComponent.h"
#include "Hud/ApexHudData.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------
// A watched hotlap (SessionKind::HotlapWatch): one AI car laps a circuit alone,
// simulated by the game (hotlap_watch::LocalWatch). The Rust tests cover the
// lap; these cover what the client does with the watch and its corners.
// -----------------------------------------------------------------------------

namespace
{
	FApexTrackCorners HotlapCorners()
	{
		FApexTrackCorners Corners;
		Corners.SessionId = TEXT("c401cf66-0e18-4c10-8bbd-18cf5e162ddd");
		Corners.TrackLengthM = 5793.0f;
		// A first corner that straddles the line, a nameless second.
		FApexTrackCorner& First = Corners.Corners.AddDefaulted_GetRef();
		First.Number = 1;
		First.Name = TEXT("Variante");
		First.EntryM = 5700.0f;
		First.ApexM = 120.0f;
		First.ExitM = 210.0f;
		FApexTrackCorner& Second = Corners.Corners.AddDefaulted_GetRef();
		Second.Number = 2;
		Second.EntryM = 900.0f;
		Second.ApexM = 950.0f;
		Second.ExitM = 1000.0f;
		Second.bLeft = true;
		return Corners;
	}

	const FApexHudValue& HotlapHudValue(const FApexHudData& Data, const TCHAR* Name)
	{
		static const FApexHudValue None;
		const FApexHudValue* Found = Data.Find(FName(Name));
		return Found ? *Found : None;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHotlapWatchSessionTest, "ApexSim.Hotlap.Session", ApexTestFlags)

bool FApexHotlapWatchSessionTest::RunTest(const FString& Parameters)
{
	UApexNetSubsystem* Net = NewObject<UApexNetSubsystem>(NewObject<UGameInstance>(GetTransientPackage()));
	FApexSessionConditions Sky;
	Sky.Weather = EApexWeather::LightRain;
	Sky.TimeOfDayMinutes = 18 * 60 + 15;
	TestFalse(TEXT("no hotlap yet"), Net->IsHotlapWatch());

	// Simulated here: nothing is sent, no server is needed.
	Net->BeginLocalHotlapWatch(TEXT("track-id"), TEXT("Zandervoort"), TEXT("Zandervoort"), Sky, TArrayView<const uint8>());
	TestTrue(TEXT("a watched hotlap"), Net->IsHotlapWatch());
	TestFalse(TEXT("not a session the player is in"), Net->IsInSession());

	// Unlisted: once its stream names it, the watch stands in for the lobby's entry.
	const FString StreamId = TEXT("c401cf66-0e18-4c10-8bbd-18cf5e162ddd");
	Net->SetLocalHotlapWatchId(StreamId);
	FApexSessionSummary Summary;
	if (TestTrue(TEXT("found by id"), Net->FindSessionById(StreamId, Summary)))
	{
		TestEqual(TEXT("its circuit file"), Summary.TrackFile, FString(TEXT("tracks/default/Zandervoort/Zandervoort.yaml")));
		TestEqual(TEXT("its circuit's name"), Summary.TrackName, FString(TEXT("Zandervoort")));
		TestEqual(TEXT("its kind"), static_cast<int32>(Summary.SessionKind), static_cast<int32>(EApexSessionKind::HotlapWatch));
		TestEqual(TEXT("its sky"), static_cast<int32>(Summary.Conditions.Weather), static_cast<int32>(EApexWeather::LightRain));
	}
	TestFalse(TEXT("no other session is found"), Net->FindSessionById(TEXT("another"), Summary));

	Net->EndLocalHotlapWatch();
	TestFalse(TEXT("ended"), Net->IsHotlapWatch());
	TestFalse(TEXT("and no longer found"), Net->FindSessionById(StreamId, Summary));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHotlapWatchHudDataTest, "ApexSim.Hotlap.HudData", ApexTestFlags)

bool FApexHotlapWatchHudDataTest::RunTest(const FString& Parameters)
{
	FApexTrackCorners Corners = HotlapCorners();

	FApexTelemetryFrame Frame;
	FApexCarTelemetry& Car = Frame.Cars.AddDefaulted_GetRef();
	Car.CarIndex = 0;
	Car.TrackProgress = 5750.0f;

	FApexHudInputs In;
	In.Frame = &Frame;
	In.LocalCarIndex = 0;
	In.TrackLengthM = Corners.TrackLengthM;
	In.GameMode = EApexGameMode::Hotlap;
	In.bSpectating = true;
	In.SpectateSource = TEXT("hotlap");
	In.bHotlapWatch = true;
	In.Corners = &Corners;

	FApexHudMemory Memory;
	FApexHudData Data;
	ApexHudData::Build(In, Memory, Data);
	TestTrue(TEXT("the hotlap scene is up"), HotlapHudValue(Data, TEXT("hotlap.active")).AsBool());
	TestEqual(TEXT("in the first corner, before the line"), HotlapHudValue(Data, TEXT("corner.number")).AsNumber(), 1.0);
	TestEqual(TEXT("its name"), HotlapHudValue(Data, TEXT("corner.name")).AsString(), FString(TEXT("Variante")));
	TestEqual(TEXT("shown as"), HotlapHudValue(Data, TEXT("corner.label")).AsString(), FString(TEXT("Variante")));
	TestEqual(TEXT("it turns right"), HotlapHudValue(Data, TEXT("corner.direction")).AsString(), FString(TEXT("right")));
	TestTrue(TEXT("inside it"), HotlapHudValue(Data, TEXT("corner.inside")).AsBool());
	TestEqual(TEXT("so none to go"), HotlapHudValue(Data, TEXT("corner.distance_m")).AsNumber(), 0.0);
	TestEqual(TEXT("two in the lap"), HotlapHudValue(Data, TEXT("corner.count")).AsNumber(), 2.0);

	// Out of it, on the way to the nameless second: "Turn 2", 600 m off.
	Frame.Cars[0].TrackProgress = 300.0f;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("coming to the second"), HotlapHudValue(Data, TEXT("corner.number")).AsNumber(), 2.0);
	TestTrue(TEXT("it has no name"), HotlapHudValue(Data, TEXT("corner.name")).IsNone());
	TestEqual(TEXT("so it is a turn"), HotlapHudValue(Data, TEXT("corner.label")).AsString(), FString(TEXT("Turn 2")));
	TestEqual(TEXT("it turns left"), HotlapHudValue(Data, TEXT("corner.direction")).AsString(), FString(TEXT("left")));
	TestFalse(TEXT("not in it yet"), HotlapHudValue(Data, TEXT("corner.inside")).AsBool());
	TestEqual(TEXT("600 m to its entry"), HotlapHudValue(Data, TEXT("corner.distance_m")).AsNumber(), 600.0, 1e-3);

	// The circuit's own turn numbers: a bend it does not number (0) has no
	// number, its name alone; the count is the highest turn.
	Corners.Corners[0].Number = 4;
	Corners.Corners[1].Number = 0;
	Corners.Corners[1].Name = TEXT("Kink");
	ApexHudData::Build(In, Memory, Data);
	TestTrue(TEXT("an unnumbered bend has no number"), HotlapHudValue(Data, TEXT("corner.number")).IsNone());
	TestEqual(TEXT("its name alone"), HotlapHudValue(Data, TEXT("corner.label")).AsString(), FString(TEXT("Kink")));
	TestEqual(TEXT("counted to the highest turn"), HotlapHudValue(Data, TEXT("corner.count")).AsNumber(), 4.0);
	Corners = HotlapCorners();

	// No corners (an older server): the data points are there, empty.
	In.Corners = nullptr;
	ApexHudData::Build(In, Memory, Data);
	TestTrue(TEXT("no corner"), HotlapHudValue(Data, TEXT("corner.number")).IsNone());
	TestFalse(TEXT("so none inside"), HotlapHudValue(Data, TEXT("corner.inside")).AsBool());

	// The setup, stock by default; streamed, the transport against the live edge.
	TestEqual(TEXT("stock"), HotlapHudValue(Data, TEXT("hotlap.setup")).AsString(), FString(TEXT("stock")));
	TestFalse(TEXT("not streamed"), HotlapHudValue(Data, TEXT("replay.live")).AsBool());
	TestTrue(TEXT("so no behind"), HotlapHudValue(Data, TEXT("replay.behind_s")).IsNone());
	In.HotlapSetup = TEXT("Low wing");
	In.bReplay = true;
	In.bReplayLive = true;
	In.bReplayPaused = true;
	In.ReplayBehindSeconds = 12.5;
	ApexHudData::Build(In, Memory, Data);
	TestEqual(TEXT("a saved setup"), HotlapHudValue(Data, TEXT("hotlap.setup")).AsString(), FString(TEXT("Low wing")));
	TestTrue(TEXT("streamed"), HotlapHudValue(Data, TEXT("replay.live")).AsBool());
	TestTrue(TEXT("held"), HotlapHudValue(Data, TEXT("replay.paused")).AsBool());
	TestEqual(TEXT("behind its edge"), HotlapHudValue(Data, TEXT("replay.behind_s")).AsNumber(), 12.5);

	// Driving, not watching: not the hotlap scene whatever the net says.
	In.bSpectating = false;
	ApexHudData::Build(In, Memory, Data);
	TestFalse(TEXT("only a watcher has the scene"), HotlapHudValue(Data, TEXT("hotlap.active")).AsBool());
	TestTrue(TEXT("nor a setup"), HotlapHudValue(Data, TEXT("hotlap.setup")).IsNone());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHotlapWatchSceneTest, "ApexSim.Hotlap.Scene", ApexTestFlags)

bool FApexHotlapWatchSceneTest::RunTest(const FString& Parameters)
{
	// The shipped scene: its six components load clean and belong to it, and
	// none of the ordinary HUD's does.
	const FString Dir = ApexRepoPaths::CodePath(TEXT("hud"));
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

	for (const TCHAR* Id : {TEXT("hotlap_header"), TEXT("hotlap_corner"), TEXT("hotlap_timing"), TEXT("hotlap_car"),
			 TEXT("hotlap_controls"), TEXT("hotlap_transport")})
	{
		const FApexHudComponentDef* Found = Components.FindByPredicate([Id](const FApexHudComponentDef& C) { return C.Id == Id; });
		TestTrue(*FString::Printf(TEXT("has %s"), Id), Found != nullptr);
		TestTrue(*FString::Printf(TEXT("%s is in the hotlap scene"), Id), Found && Found->Scene == TEXT("hotlap_watch"));
		TestTrue(*FString::Printf(TEXT("%s is on by default"), Id), Found && Found->bDefaultEnabled);
	}
	for (const FApexHudComponentDef& Component : Components)
	{
		if (!Component.Id.StartsWith(TEXT("hotlap_")))
		{
			TestTrue(*FString::Printf(TEXT("%s is the ordinary HUD"), *Component.Id), Component.Scene.IsEmpty());
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
