#include "ApexTestCommon.h"
#include "ApexNetSubsystem.h"
#include "Engine/GameInstance.h"
#include "Hud/ApexHudComponent.h"
#include "Hud/ApexHudData.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------
// A watched hotlap (SessionKind::HotlapWatch): one AI car laps a circuit alone,
// the creator spectates. The server tests cover the lap; these cover what the
// client does with the session and with the corners that come with it.
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
	// No connection here: the requests and the lobby refresh the join asks for are dropped.
	AddExpectedError(TEXT("Dropped an outbound message"), EAutomationExpectedErrorFlags::Contains, 0);

	UApexNetSubsystem* Net = NewObject<UApexNetSubsystem>(NewObject<UGameInstance>(GetTransientPackage()));
	FApexSessionConditions Sky;
	Sky.Weather = EApexWeather::LightRain;
	Sky.TimeOfDayMinutes = 18 * 60 + 15;
	Net->CreateHotlapWatch(TEXT("track-id"), TEXT("Zandervoort"), TEXT("Zandvoort"), Sky);
	TestFalse(TEXT("not a hotlap until the server says so"), Net->IsHotlapWatch());

	const FString SessionId = TEXT("c401cf66-0e18-4c10-8bbd-18cf5e162ddd");
	FApexServerMessage Joined;
	Joined.Type = EApexServerMessageType::SessionJoined;
	Joined.SessionId = SessionId;
	Joined.GridPosition = 0;
	Joined.SessionKind = EApexSessionKind::HotlapWatch;
	Joined.Conditions = Sky;
	Net->HandleMessageForTest(Joined);

	TestTrue(TEXT("a watched hotlap"), Net->IsHotlapWatch());
	TestTrue(TEXT("watched as a spectator: no car of ours"), Net->IsSessionSpectator());
	TestEqual(TEXT("its sky"), static_cast<int32>(Net->GetSessionConditions().Weather), static_cast<int32>(EApexWeather::LightRain));

	// It is unlisted, so the lobby cannot name its circuit: the request did.
	FApexSessionSummary Summary;
	if (TestTrue(TEXT("found by id"), Net->FindSessionById(SessionId, Summary)))
	{
		TestEqual(TEXT("its circuit file"), Summary.TrackFile, FString(TEXT("tracks/default/Zandvoort.yaml")));
		TestEqual(TEXT("its circuit's name"), Summary.TrackName, FString(TEXT("Zandervoort")));
		TestEqual(TEXT("its kind"), static_cast<int32>(Summary.SessionKind), static_cast<int32>(EApexSessionKind::HotlapWatch));
	}
	TestFalse(TEXT("no other session is found"), Net->FindSessionById(TEXT("another"), Summary));

	// The corners come after the join.
	FApexServerMessage Corners;
	Corners.Type = EApexServerMessageType::TrackCorners;
	Corners.TrackCorners = HotlapCorners();
	Net->HandleMessageForTest(Corners);
	TestEqual(TEXT("two corners"), Net->GetTrackCorners().Corners.Num(), 2);

	// Another car, another sky: the server replaces the session without a
	// word, and the next join starts from the lobby with no corners yet.
	FApexServerMessage Again = Joined;
	Again.SessionId = TEXT("0c6fb2a4-1f8e-4a52-9d3b-2b3c4d5e6f70");
	Net->CreateHotlapWatch(TEXT("track-id"), TEXT("Zandervoort"), TEXT("Zandvoort"), Sky);
	Net->HandleMessageForTest(Again);
	TestTrue(TEXT("still a hotlap"), Net->IsHotlapWatch());
	TestEqual(TEXT("the old corners went"), Net->GetTrackCorners().Corners.Num(), 0);
	TestFalse(TEXT("the old session is no longer found"), Net->FindSessionById(SessionId, Summary));
	TestTrue(TEXT("the new one is"), Net->FindSessionById(Again.SessionId, Summary));

	FApexServerMessage Left;
	Left.Type = EApexServerMessageType::SessionLeft;
	Net->HandleMessageForTest(Left);
	TestFalse(TEXT("left"), Net->IsHotlapWatch());
	TestFalse(TEXT("and no longer found"), Net->FindSessionById(Again.SessionId, Summary));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHotlapWatchHudDataTest, "ApexSim.Hotlap.HudData", ApexTestFlags)

bool FApexHotlapWatchHudDataTest::RunTest(const FString& Parameters)
{
	const FApexTrackCorners Corners = HotlapCorners();

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

	// No corners (an older server): the data points are there, empty.
	In.Corners = nullptr;
	ApexHudData::Build(In, Memory, Data);
	TestTrue(TEXT("no corner"), HotlapHudValue(Data, TEXT("corner.number")).IsNone());
	TestFalse(TEXT("so none inside"), HotlapHudValue(Data, TEXT("corner.inside")).AsBool());

	// Driving, not watching: not the hotlap scene whatever the net says.
	In.bSpectating = false;
	ApexHudData::Build(In, Memory, Data);
	TestFalse(TEXT("only a watcher has the scene"), HotlapHudValue(Data, TEXT("hotlap.active")).AsBool());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexHotlapWatchSceneTest, "ApexSim.Hotlap.Scene", ApexTestFlags)

bool FApexHotlapWatchSceneTest::RunTest(const FString& Parameters)
{
	// The shipped scene: its five components load clean and belong to it, and
	// none of the ordinary HUD's does.
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content/hud")));
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
			 TEXT("hotlap_controls")})
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
