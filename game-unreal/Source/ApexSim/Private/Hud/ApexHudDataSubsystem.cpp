#include "Hud/ApexHudDataSubsystem.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
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

float UApexHudDataSubsystem::CatalogTrackLengthM() const
{
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	FApexTrackCatalogRow Row;
	return Flow && Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row) ? Row.LengthM : 0.0f;
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
	const int32 LocalIndex = Net->GetLocalCarIndex();
	if (const FApexCarTelemetry* Local = Frame.Cars.FindByPredicate(
			[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; }))
	{
		Memory.SampleLap(*Local, CatalogTrackLengthM());
	}
}

void UApexHudDataSubsystem::Refresh()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	const UApexSettingsSubsystem* Settings = GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>();
	const UApexSettingsSave* Save = Settings ? Settings->Get() : nullptr;

	FApexHudInputs In;
	In.TimeSeconds = bRaceActive ? FPlatformTime::Seconds() - RaceStartSeconds : 0.0;
	In.bImperial = Save && Save->Units == EApexUnits::Imperial;
	In.bFullDetail = !Save || Save->HudDetail == EApexHudDetail::All;
	In.DamageLevel = Save ? Save->Damage : EApexDamageLevel::Full;
	In.TrackLengthM = CatalogTrackLengthM();

	if (Net)
	{
		In.Frame = &Net->GetLatestTelemetry();
		In.LocalCarIndex = Net->GetLocalCarIndex();
		In.Roster = &Net->GetSessionRoster();
		In.Timing = &Net->GetTimingBoard();
		In.Sectors = &Net->GetTrackSectors();
		In.GameMode = Net->GetGameMode();
		In.ModeName = UApexMenuFlowSubsystem::GetGameModeName(In.GameMode);
		In.PingMs = Net->GetPingMs();
		In.Conditions = Net->GetSessionConditions();
		In.bHasConditions = Net->IsInSession() || Net->IsInDemoSession();
		// The level this car runs at: the player's setting, unless the session
		// pins full damage (the server applies the same rule).
		if (Net->IsInSession() && !Net->GetAllowedAssists().bDamage)
		{
			In.DamageLevel = EApexDamageLevel::Full;
		}
		FApexSessionSummary Session;
		if (Net->FindSessionById(Net->GetCurrentSessionId(), Session) && !Session.TrackName.IsEmpty())
		{
			In.TrackName = Session.TrackName;
		}
	}

	if (Flow)
	{
		// A hotlap has no distance: laps are counted, never counted down.
		In.LapLimit = In.GameMode == EApexGameMode::Hotlap ? 0 : Flow->CreateLapLimit;
		if (In.TrackName.IsEmpty())
		{
			FApexTrackCatalogRow Row;
			if (Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row))
			{
				In.TrackName = Row.DisplayName;
			}
		}
		// The car's own figures, read once per car: its tyres' working window
		// and its rev range, neither of which is on the wire.
		if (Flow->GetPendingCarId() != CatalogCarId)
		{
			CatalogCarId = Flow->GetPendingCarId();
			FApexCarCatalogRow CarRow;
			const bool bRow = Flow->GetCarCatalogRow(CatalogCarId, CarRow);
			TyreOptimalC = bRow ? CarRow.TyreOptimalC : 90.0f;
			TyreWindowC = bRow ? CarRow.TyreWindowC : 10.0f;
			RedlineRpm = bRow ? CarRow.EngineSound.RedlineRpm : 0.0f;
			LimiterRpm = bRow ? CarRow.EngineSound.LimiterRpm : 0.0f;
			CarName = bRow ? CarRow.DisplayName : FString();
		}
	}
	In.TyreOptimalC = TyreOptimalC;
	In.TyreWindowC = TyreWindowC;
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
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<UApexMenuFlowSubsystem>();
	if (!Net || !Flow)
	{
		return TrackOutline;
	}
	// The outline arrives with the lobby state, which is broadcast every two
	// seconds and only carries points when the codec is parsing them; keep
	// asking until it has, and again when the track changes.
	const FString TrackId = Flow->GetPendingTrackId();
	if (TrackOutline.Num() < 2 || TrackId != TrackOutlineId)
	{
		FApexTrackConfigSummary Track;
		if (Net->FindTrackById(TrackId, Track) && Track.Centerline.Num() > 1)
		{
			TrackOutline = Track.Centerline;
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
	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return;
	}
	const int32 LocalIndex = Net->GetLocalCarIndex();
	Out.Reserve(Net->GetLatestTelemetry().Cars.Num());
	for (const FApexCarTelemetry& Car : Net->GetLatestTelemetry().Cars)
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
