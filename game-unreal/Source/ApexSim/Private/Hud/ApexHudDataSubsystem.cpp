#include "Hud/ApexHudDataSubsystem.h"

#include "ApexDemoModeSubsystem.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "ApexSpectatorSubsystem.h"
#include "Input/ApexInputConfig.h"
#include "Race/ApexRaceDirector.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UI/ApexMinimapWidget.h"

namespace
{
	FAutoConsoleCommandWithWorldAndArgs HudDataCommand(TEXT("apexsim.hud.Data"),
		TEXT("Print every HUD data point and its value (the names a content/hud component can read). ")
		TEXT("An argument keeps only the lines containing it: apexsim.hud.Data tyre"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			UApexHudDataSubsystem* Hud = GameInstance ? GameInstance->GetSubsystem<UApexHudDataSubsystem>() : nullptr;
			if (!Hud)
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.hud.Data: no game instance"));
				return;
			}
			// Outside a race nothing refreshes the data; build it now so the
			// catalogue can be read from the menu too.
			if (!Hud->IsRaceActive())
			{
				Hud->Refresh();
			}
			const TArray<FString> Lines = Hud->DescribeData(Args.Num() > 0 ? Args[0] : FString());
			for (const FString& Line : Lines)
			{
				UE_LOG(LogApexSim, Display, TEXT("%s"), *Line);
			}
			UE_LOG(LogApexSim, Display, TEXT("apexsim.hud.Data: %d line(s)"), Lines.Num());
		}));

	FString HudDescribeValue(const FApexHudValue& Value)
	{
		switch (Value.Type)
		{
		case EApexHudValueType::None: return TEXT("null");
		case EApexHudValueType::String: return FString::Printf(TEXT("'%s'"), *Value.String);
		default: return Value.AsString();
		}
	}
}

void UApexHudDataSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	Collection.InitializeDependency<UApexSettingsSubsystem>();
	Collection.InitializeDependency<UApexMenuFlowSubsystem>();
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnTelemetry.AddDynamic(this, &UApexHudDataSubsystem::HandleTelemetry);
	}
}

void UApexHudDataSubsystem::Deinitialize()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnTelemetry.RemoveDynamic(this, &UApexHudDataSubsystem::HandleTelemetry);
	}
	Super::Deinitialize();
}

UApexNetSubsystem* UApexHudDataSubsystem::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

void UApexHudDataSubsystem::SetWatchState(const FString& Source, const FString& InTowerMode)
{
	WatchSource = Source;
	TowerMode = InTowerMode;
}

void UApexHudDataSubsystem::ResolveTrack(bool bBackdrop)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GameInstance->GetSubsystem<UApexMenuFlowSubsystem>();
	FString TrackId;
	FString Name;
	float LengthM = 0.0f;
	int32 LapLimit = -1;

	if (bBackdrop)
	{
		// A stream names its own circuit, distance and length; a demo
		// session is on the circuit the demo chose.
		const UApexSpectatorSubsystem* Spectator = GameInstance->GetSubsystem<UApexSpectatorSubsystem>();
		if (Spectator && Spectator->GetSource() != EApexSpectatorSource::None && Spectator->HasHeader())
		{
			const FApexStreamHeader& Header = Spectator->GetHeader();
			TrackId = Header.Track.TrackId;
			Name = Header.Track.DisplayName;
			LengthM = Header.Track.LengthM;
			LapLimit = Header.LapLimit;
		}
		else if (const UApexDemoModeSubsystem* Demo = GameInstance->GetSubsystem<UApexDemoModeSubsystem>())
		{
			TrackId = Demo->GetDemoTrackId();
		}
	}
	else if (Net && Net->IsInSession())
	{
		FApexSessionSummary Session;
		if (Net->FindSessionById(Net->GetCurrentSessionId(), Session))
		{
			TrackId = Session.TrackId;
			Name = Session.TrackName;
			if (Session.LapLimit > 0)
			{
				LapLimit = Session.LapLimit;
			}
		}
		// A timed race has no distance, listed or not.
		if (Net->GetSessionRaceSeconds() > 0)
		{
			LapLimit = 0;
		}
	}
	if (TrackId.IsEmpty() && Flow)
	{
		TrackId = Flow->GetPendingTrackId();
	}
	FApexTrackCatalogRow Row;
	if (Flow && !TrackId.IsEmpty() && Flow->GetTrackCatalogRow(TrackId, Row))
	{
		if (LengthM <= 0.0f)
		{
			LengthM = Row.LengthM;
		}
		if (Name.IsEmpty())
		{
			Name = Row.DisplayName;
		}
	}
	HudTrackId = TrackId;
	HudTrackName = Name;
	HudTrackLengthM = LengthM;
	HudLapLimit = LapLimit;
}

void UApexHudDataSubsystem::RefreshCarNames()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	if (!Net || !Flow)
	{
		return;
	}
	const FApexSessionRoster& Roster = Net->GetSessionRoster();
	FString Key = Roster.SessionId;
	for (const FApexRosterEntry& Entry : Roster.Entries)
	{
		Key += FString::Printf(TEXT("|%d=%s"), Entry.CarIndex, *Entry.CarConfigId);
	}
	if (Key == CarNamesKey)
	{
		return;
	}
	CarNamesKey = Key;
	CarNames.Reset();
	CarCompounds.Reset();
	CarHalfSizes.Reset();
	TMap<FString, FApexCarCatalogRow> ByCarId;
	for (const FApexRosterEntry& Entry : Roster.Entries)
	{
		FApexCarCatalogRow* Known = ByCarId.Find(Entry.CarConfigId);
		if (!Known)
		{
			FApexCarCatalogRow Row;
			Known = &ByCarId.Add(Entry.CarConfigId, Flow->GetCarCatalogRow(Entry.CarConfigId, Row) ? Row : FApexCarCatalogRow());
		}
		CarNames.Add(Entry.CarIndex, Known->DisplayName);
		CarHalfSizes.Add(Entry.CarIndex, ApexHudData::CarHalfSize(Known->Wheels));
		TArray<FString>& Names = CarCompounds.Add(Entry.CarIndex);
		for (const FApexCompoundSpec& Compound : Known->Compounds)
		{
			Names.Add(Compound.Name);
		}
	}
}

void UApexHudDataSubsystem::SetRaceActive(bool bActive)
{
	if (bActive && !bRaceActive)
	{
		// A new race starts with no history: keeping the previous session's
		// reference lap would show a delta against a lap of another circuit.
		Memory.Reset();
		RaceStartSeconds = FPlatformTime::Seconds();
		CatalogCarId.Reset();
		TrackOutline.Reset();
		TrackOutlineId.Reset();
	}
	bRaceActive = bActive;
}

void UApexHudDataSubsystem::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	const UApexNetSubsystem* Net = GetNet();
	if (!bRaceActive || !Net)
	{
		return;
	}
	// The car the HUD is about, as the last refresh found it: the player's,
	// or the one being watched.
	const int32 LocalIndex = LastLocalIndex;
	if (const FApexCarTelemetry* Local = Frame.Cars.FindByPredicate(
			[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; }))
	{
		Memory.SampleLap(*Local, HudTrackLengthM);
	}
}

void UApexHudDataSubsystem::SetPreview(bool bInPreview)
{
	bPreview = bInPreview;
	if (!bPreview)
	{
		Preview.Reset();
	}
}

void UApexHudDataSubsystem::Refresh()
{
	if (IsShowingPreview())
	{
		if (!Preview)
		{
			Preview = MakeUnique<FApexHudPreview>();
		}
		FApexHudInputs& PreviewInputs = Preview->Inputs;
		PreviewInputs.TimeSeconds = FPlatformTime::Seconds();
		if (const UApexSettingsSubsystem* PreviewSettings = GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>())
		{
			if (const UApexSettingsSave* PreviewSave = PreviewSettings->Get())
			{
				PreviewInputs.bImperial = PreviewSave->Units == EApexUnits::Imperial;
			}
		}
		LastFrame = &Preview->Frame;
		LastLocalIndex = PreviewInputs.LocalCarIndex;
		ApexHudData::Build(PreviewInputs, Preview->Memory, Data);
		return;
	}

	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	const UApexSettingsSubsystem* Settings = GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>();
	const UApexSettingsSave* Save = Settings ? Settings->Get() : nullptr;
	const AApexRaceDirector* Director = AApexRaceDirector::Find(GetGameInstance()->GetWorld());
	const bool bSpectating = Director && Director->IsSpectating();
	const bool bBackdrop = Net && Net->IsInDemoSession();

	FApexHudInputs In;
	In.TimeSeconds = bRaceActive ? FPlatformTime::Seconds() - RaceStartSeconds : 0.0;
	In.bImperial = Save && Save->Units == EApexUnits::Imperial;
	In.bFullDetail = !Save || Save->HudDetail == EApexHudDetail::All;
	In.bGamepad = bGamepad;
	In.bSpectating = bSpectating;
	if (bSpectating)
	{
		// The watch keys as bound for the device in use, for the hints.
		if (Settings)
		{
			const int32 Slot = bGamepad ? ApexInput::Slot::Gamepad : ApexInput::Slot::Keyboard;
			auto Name = [Settings, Slot](FName ActionId) { return ApexInput::GetKeyDisplayName(Settings->GetBoundKey(ActionId, Slot)); };
			using namespace ApexInput::Actions;
			In.WatchKeys.Cars = Name(WatchPreviousCar) + TEXT(" ") + Name(WatchNextCar);
			In.WatchKeys.Camera = Name(WatchCamera);
			In.WatchKeys.Auto = Name(WatchAuto);
			In.WatchKeys.Tower = Name(WatchTower);
			In.WatchKeys.Overlay = Name(WatchOverlay);
			In.WatchKeys.NextRace = Name(WatchNextRace);
			In.WatchKeys.Leave = Name(WatchLeave);
			In.WatchKeys.PlayPause = Name(WatchPlayPause);
			In.WatchKeys.Seek = Name(WatchSeekBack) + TEXT(" ") + Name(WatchSeekForward);
			In.WatchKeys.Speed = Name(WatchSlower) + TEXT(" ") + Name(WatchFaster);
			In.WatchKeys.PauseMenu = Name(PauseMenu);
			In.WatchKeys.Setup = Name(WatchSetup);
			In.WatchKeys.ReplayLap = Name(WatchReplayLap);
			In.WatchKeys.Ghost = Name(WatchGhost);
		}
		In.SpectateSource = WatchSource.IsEmpty() ? FString(bBackdrop ? TEXT("demo") : TEXT("live")) : WatchSource;
		In.SpectateCamera = ApexSpectate::CameraName(Director->GetSpectatorCamera());
		In.bSpectateAuto = Director->IsSpectatorAuto();
		const UApexDemoModeSubsystem* Demo = GetGameInstance()->GetSubsystem<UApexDemoModeSubsystem>();
		const UApexSpectatorSubsystem* Spectator = GetGameInstance()->GetSubsystem<UApexSpectatorSubsystem>();
		if (bBackdrop && Demo && Demo->IsPlayingReplay() && Spectator)
		{
			In.bReplay = true;
			In.ReplaySeconds = Spectator->GetPlaybackSeconds();
			In.ReplayDurationSeconds = Spectator->GetDurationSeconds();
			In.ReplayRate = Spectator->GetPlaybackRate();
			In.bReplayPaused = Spectator->IsPaused();
			In.bReplayEnded = Spectator->IsAtEnd();
		}
		else if (bBackdrop && Demo && Demo->IsWatchingLive() && Spectator && Spectator->CanTimeshift())
		{
			// A live stream held back: the same transport, against its edge.
			In.bReplay = true;
			In.bReplayLive = true;
			In.ReplaySeconds = Spectator->GetPlaybackSeconds();
			In.ReplayDurationSeconds = Spectator->GetDurationSeconds();
			In.ReplayRate = Spectator->GetPlaybackRate();
			In.bReplayPaused = Spectator->IsPaused();
			In.ReplayBehindSeconds = Spectator->GetSecondsBehindLive();
		}
	}
	In.SpectateTowerMode = TowerMode;

	ResolveTrack(bBackdrop);
	In.TrackLengthM = HudTrackLengthM;
	In.TrackName = HudTrackName;

	LastFrame = nullptr;
	const int32 PreviousLocalIndex = LastLocalIndex;
	LastLocalIndex = -1;
	if (Net)
	{
		In.Frame = &Net->GetLatestTelemetry();
		// Watching, the HUD is about the car on screen.
		In.LocalCarIndex = bSpectating ? Director->GetFocusCarIndex() : Net->GetLocalCarIndex();
		FApexGhostSample Now, Before, Earlier;
		if (Director && Director->IsGhostReplayActive() && Director->GetGhostReplaySamples(Now, Before, Earlier))
		{
			// The lap again: the HUD is about the ghost being driven round.
			GhostReplayFrame = *In.Frame;
			const int32 Index = In.LocalCarIndex;
			if (FApexCarTelemetry* Car = GhostReplayFrame.Cars.FindByPredicate(
					[Index](const FApexCarTelemetry& C) { return C.CarIndex == Index; }))
			{
				auto Speed = [](const FApexGhostSample& A, const FApexGhostSample& B)
				{
					// A streamed ghost has its pose only: its speed is how far it went.
					if (B.SpeedMps > 0.0f || B.TimeMs <= A.TimeMs)
					{
						return B.SpeedMps;
					}
					return static_cast<float>(FVector::Dist(A.Position, B.Position) * 1000.0 / (B.TimeMs - A.TimeMs));
				};
				const float SpeedNow = Speed(Before, Now);
				const float SpeedBefore = Speed(Earlier, Before);
				const float Seconds = FMath::Max(0.001f, (Now.TimeMs - Before.TimeMs) / 1000.0f);
				const float Accel = (SpeedNow - SpeedBefore) / Seconds;
				Car->SpeedMps = SpeedNow;
				Car->Steering = Now.Steering;
				Car->Throttle = FMath::Clamp(Accel / 4.0f, 0.0f, 1.0f);
				Car->Brake = FMath::Clamp(-Accel / 15.0f, 0.0f, 1.0f);
				Car->CurrentLapTimeMs = Now.TimeMs;
				if (Now.Gear != 0)
				{
					Car->Gear = Now.Gear;
					Car->EngineRpm = Now.EngineRpm;
				}
				else if (const TPair<int32, float>* Seen = GearAtSpeed.Find(FMath::RoundToInt(SpeedNow / 2.0f)))
				{
					Car->Gear = Seen->Key;
					Car->EngineRpm = Seen->Value;
				}
				In.Frame = &GhostReplayFrame;
			}
		}
		else if (const FApexCarTelemetry* Live = In.Frame->Cars.FindByPredicate(
					 [Index = In.LocalCarIndex](const FApexCarTelemetry& C) { return C.CarIndex == Index; }))
		{
			if (Live->Gear > 0)
			{
				GearAtSpeed.Add(FMath::RoundToInt(Live->SpeedMps / 2.0f), {Live->Gear, Live->EngineRpm});
			}
		}
		LastFrame = In.Frame;
		LastLocalIndex = In.LocalCarIndex;
		In.Roster = &Net->GetSessionRoster();
		In.Timing = &Net->GetTimingBoard();
		In.PitServices = &Net->GetPitServices();
		In.OwnPitBox = bSpectating ? nullptr : &Net->GetOwnPitBox();
		In.Sectors = &Net->GetTrackSectors();
		In.Corners = &Net->GetTrackCorners();
		In.bHotlapWatch = Net->IsHotlapWatch();
		In.HotlapSetup = HotlapSetup;
		// The backdrop's mode is its frames'; the net subsystem keeps a demo out of its own.
		In.GameMode = bBackdrop && In.Frame->GameMode != EApexGameMode::Lobby ? In.Frame->GameMode : Net->GetGameMode();
		In.ModeName = UApexMenuFlowSubsystem::GetGameModeName(In.GameMode);
		In.PingMs = Net->GetPingMs();
		In.Conditions = Net->GetSessionConditions();
		In.bHasConditions = Net->IsInSession() || Net->IsInDemoSession();
		In.SessionId = Net->GetCurrentSessionId();
		// The session's damage rule, the same for every car.
		In.DamageLevel = Net->GetSessionDamage();
		// The race's pit rule, the same for every car.
		In.MandatoryStops = Net->GetSessionMandatoryStops();
		In.bCompoundChange = Net->GetSessionCompoundChange();
		In.QualifyingLapsLeft = Net->GetQualifyingLapsLeft();
		RefreshCarNames();
		In.CarNames = &CarNames;
		In.CarCompounds = &CarCompounds;
		In.CarHalfSizes = &CarHalfSizes;
	}
	if (PreviousLocalIndex >= 0 && LastLocalIndex >= 0 && PreviousLocalIndex != LastLocalIndex)
	{
		// Another car: its delta, fuel and damage start over.
		Memory.ResetForNewCar();
	}

	if (Flow)
	{
		// A hotlap (or qualifying) has no distance: laps are counted, never
		// counted down. The race on screen says its own; before it does, the
		// host's choice.
		In.LapLimit = ApexIsGarageMode(In.GameMode) ? 0
			: HudLapLimit >= 0 ? HudLapLimit
			: (bSpectating || bBackdrop) ? 0
			: Flow->EffectiveRaceSeconds() > 0 ? 0
			: Flow->CreateLapLimit;
		// The car's own figures, read once per car: its tyres' working window
		// and its rev range, neither of which is on the wire. Watching, the
		// car on screen; driving, the player's.
		FString CarId = Flow->GetPendingCarId();
		if (bSpectating && Net)
		{
			const int32 Watched = In.LocalCarIndex;
			const FApexRosterEntry* Entry = Net->GetSessionRoster().Entries.FindByPredicate(
				[Watched](const FApexRosterEntry& Candidate) { return Candidate.CarIndex == Watched; });
			CarId = Entry ? Entry->CarConfigId : FString();
		}
		if (CarId != CatalogCarId)
		{
			CatalogCarId = CarId;
			GearAtSpeed.Reset();
			FApexCarCatalogRow CarRow;
			const bool bRow = Flow->GetCarCatalogRow(CatalogCarId, CarRow);
			TyreOptimalC = bRow ? CarRow.TyreOptimalC : 90.0f;
			TyreWindowC = bRow ? CarRow.TyreWindowC : 10.0f;
			bCarbonBrakes = bRow && CarRow.HasCarbonBrakes();
			RedlineRpm = bRow ? CarRow.EngineSound.RedlineRpm : 0.0f;
			LimiterRpm = bRow ? CarRow.EngineSound.LimiterRpm : 0.0f;
			CarName = bRow ? CarRow.DisplayName : FString();
		}
	}
	In.TyreOptimalC = TyreOptimalC;
	In.TyreWindowC = TyreWindowC;
	In.bCarbonBrakes = bCarbonBrakes;
	// The driver's car's compounds by name, from the garage's sheet. Watching
	// another car the sheet is still the driver's, so the defaults apply.
	In.CompoundNames = Net && !bSpectating && Net->GetCarSetupSheet().HasCompounds() ? &Net->GetCarSetupSheet().Compounds : nullptr;
	In.RedlineRpm = RedlineRpm;
	In.LimiterRpm = LimiterRpm;
	In.CarName = CarName;

	ApexHudData::Build(In, Memory, Data);
}

float UApexHudDataSubsystem::GetNumber(FName Name) const
{
	const FApexHudValue* Value = Data.Find(Name);
	return Value ? static_cast<float>(Value->AsNumber()) : 0.0f;
}

FString UApexHudDataSubsystem::GetText(FName Name) const
{
	const FApexHudValue* Value = Data.Find(Name);
	return Value ? Value->AsString() : FString();
}

bool UApexHudDataSubsystem::GetBool(FName Name) const
{
	const FApexHudValue* Value = Data.Find(Name);
	return Value && Value->AsBool();
}

bool UApexHudDataSubsystem::HasValue(FName Name) const
{
	const FApexHudValue* Value = Data.Find(Name);
	return Value && !Value->IsNone();
}

TArray<FString> UApexHudDataSubsystem::DescribeData(const FString& Filter) const
{
	TArray<FString> Lines;
	for (const TPair<FName, FApexHudValue>& Pair : Data.Values)
	{
		Lines.Add(FString::Printf(TEXT("%s = %s"), *Pair.Key.ToString(), *HudDescribeValue(Pair.Value)));
	}
	for (const TPair<FName, TArray<FName>>& List : ApexHudData::ListFields())
	{
		const TArray<FApexHudRecord>* Rows = Data.FindList(List.Key);
		const int32 Count = Rows ? Rows->Num() : 0;
		Lines.Add(FString::Printf(TEXT("%s: list of %d, fields %s"), *List.Key.ToString(), Count,
			*FString::JoinBy(List.Value, TEXT(", "), [](const FName& Field) { return Field.ToString(); })));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			TArray<FString> Fields;
			for (const FName& Field : List.Value)
			{
				const FApexHudValue* Value = (*Rows)[Index].Find(Field);
				Fields.Add(FString::Printf(TEXT("%s=%s"), *Field.ToString(), Value ? *HudDescribeValue(*Value) : TEXT("null")));
			}
			Lines.Add(FString::Printf(TEXT("%s[%d] %s"), *List.Key.ToString(), Index, *FString::Join(Fields, TEXT(" "))));
		}
	}
	if (!Filter.IsEmpty())
	{
		Lines.RemoveAll([&Filter](const FString& Line) { return !Line.Contains(Filter); });
	}
	Lines.Sort();
	return Lines;
}

const TArray<FVector2D>& UApexHudDataSubsystem::GetTrackOutline()
{
	if (IsShowingPreview() && Preview)
	{
		return Preview->Outline;
	}
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	if (!Net || !Flow)
	{
		return TrackOutline;
	}
	// The outline arrives with the lobby state, which is broadcast every two
	// seconds and only carries points when the codec is parsing them; keep
	// asking until it has, and again when the track changes. A stream played
	// with no server brings its own path.
	const FString TrackId = HudTrackId.IsEmpty() ? Flow->GetPendingTrackId() : HudTrackId;
	if (TrackOutline.Num() < 2 || TrackId != TrackOutlineId)
	{
		FApexTrackConfigSummary Track;
		const UApexSpectatorSubsystem* Spectator = GetGameInstance()->GetSubsystem<UApexSpectatorSubsystem>();
		if (Net->FindTrackById(TrackId, Track) && Track.Centerline.Num() > 1)
		{
			TrackOutline = Track.Centerline;
			TrackOutlineId = TrackId;
		}
		else if (Net->IsInDemoSession() && Spectator && Spectator->GetSource() != EApexSpectatorSource::None
			&& Spectator->GetCenterline().Num() > 1)
		{
			TrackOutline = Spectator->GetCenterline();
			TrackOutlineId = TrackId;
		}
		else if (TrackId != TrackOutlineId)
		{
			TrackOutline.Reset();
		}
	}
	return TrackOutline;
}

void UApexHudDataSubsystem::MakeMinimapBlips(TArray<FApexMinimapBlip>& Out, const FLinearColor& LocalColour) const
{
	if (!LastFrame)
	{
		return;
	}
	const int32 LocalIndex = LastLocalIndex;
	Out.Reserve(LastFrame->Cars.Num());
	for (const FApexCarTelemetry& Car : LastFrame->Cars)
	{
		FApexMinimapBlip Blip;
		Blip.Position = FVector2D(Car.Position.X, Car.Position.Y);
		Blip.bIsLocal = Car.CarIndex == LocalIndex;
		// Hue by index rather than a palette lookup: the field size is not known
		// until the roster lands, and every car has to get a colour.
		Blip.Colour = Blip.bIsLocal ? LocalColour : FLinearColor::MakeFromHSV8(static_cast<uint8>((Car.CarIndex * 47) % 255), 140, 235);
		Out.Add(Blip);
	}
}
