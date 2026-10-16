#include "ApexPitPlanSubsystem.h"

#include "ApexNetSubsystem.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Engine/GameInstance.h"

namespace
{
	/** The five default compounds when the setup sheet named none. */
	constexpr int32 DefaultCompoundCount = 5;

	/** The worst of the four pads' wear, percent; negative when unknown. */
	float WorstPadWear(const FApexCarTelemetry& Car)
	{
		float Worst = -1.0f;
		for (const float Wear : Car.BrakeWearPct)
		{
			Worst = FMath::Max(Worst, Wear);
		}
		return Worst;
	}
}

// ---------------------------------------------------------------------------
// The panel's choices
// ---------------------------------------------------------------------------

int32 ApexPitPlan::TyreChoice(const FApexPitPlan& Plan)
{
	return Plan.bKeepTyres ? 0 : Plan.Compound < 0 ? 1 : Plan.Compound + 2;
}

void ApexPitPlan::SetTyreChoice(FApexPitPlan& Plan, int32 Choice)
{
	Plan.bKeepTyres = Choice <= 0;
	Plan.Compound = Choice >= 2 ? Choice - 2 : -1;
}

int32 ApexPitPlan::TyreChoiceCount(const TArray<FString>& Compounds)
{
	return 2 + (Compounds.Num() > 0 ? Compounds.Num() : DefaultCompoundCount);
}

FString ApexPitPlan::TyreChoiceLabel(int32 Choice, const TArray<FString>& Compounds)
{
	if (Choice <= 0)
	{
		return TEXT("KEEP");
	}
	if (Choice == 1)
	{
		return TEXT("SETUP'S");
	}
	const FString Name = FApexCarTelemetry::CompoundNameFrom(Compounds, Choice - 2);
	return Name.IsEmpty() ? FString::Printf(TEXT("SET %d"), Choice - 1) : Name;
}

int32 ApexPitPlan::FuelChoice(const FApexPitPlan& Plan)
{
	return Plan.FuelL < 0.0f ? -1 : FMath::Clamp(FMath::RoundToInt(Plan.FuelL / FuelStepL), 0, FuelMaxSteps);
}

void ApexPitPlan::SetFuelChoice(FApexPitPlan& Plan, int32 Choice)
{
	Plan.FuelL = Choice < 0 ? -1.0f : FMath::Min(Choice, FuelMaxSteps) * FuelStepL;
}

FString ApexPitPlan::FuelChoiceLabel(int32 Choice)
{
	return Choice < 0 ? FString(TEXT("TO THE END")) : Choice == 0 ? FString(TEXT("NONE"))
		: FString::Printf(TEXT("+%d L"), FMath::RoundToInt(Choice * FuelStepL));
}

FString ApexPitPlan::RepairChoiceLabel(int32 Choice)
{
	return Choice > 0 ? TEXT("LEAVE IT") : TEXT("REPAIR");
}

int32 ApexPitPlan::PadsChoice(const FApexPitPlan& Plan)
{
	return Plan.Pads < 0 ? 0 : Plan.Pads == 0 ? 1 : 2;
}

void ApexPitPlan::SetPadsChoice(FApexPitPlan& Plan, int32 Choice)
{
	Plan.Pads = Choice <= 0 ? -1 : Choice == 1 ? 0 : 1;
}

FString ApexPitPlan::PadsChoiceLabel(int32 Choice)
{
	return Choice <= 0 ? TEXT("CREW'S CALL") : Choice == 1 ? TEXT("KEEP") : TEXT("NEW");
}

int32 ApexPitPlan::PadSetChoice(const FApexPitPlan& Plan)
{
	return Plan.bChoosePadSet ? FMath::Clamp(Plan.PadSet, -1, 1) + 2 : 0;
}

void ApexPitPlan::SetPadSetChoice(FApexPitPlan& Plan, int32 Choice)
{
	Plan.bChoosePadSet = Choice > 0;
	Plan.PadSet = Choice > 0 ? FMath::Clamp(Choice - 2, -1, 1) : 0;
}

FString ApexPitPlan::PadSetChoiceLabel(int32 Choice)
{
	return Choice <= 0 ? TEXT("SETUP'S") : Choice == 1 ? TEXT("ENDURANCE") : Choice == 2 ? TEXT("STANDARD") : TEXT("SPRINT");
}

bool ApexPitPlan::IsCrewsCall(const FApexPitPlan& Plan)
{
	FApexPitPlan Crew;
	Crew.bBoxThisLap = Plan.bBoxThisLap;
	return TyreChoice(Plan) == TyreChoice(Crew) && FuelChoice(Plan) == FuelChoice(Crew) && Plan.bNoRepair == Crew.bNoRepair
		&& PadsChoice(Plan) == PadsChoice(Crew) && PadSetChoice(Plan) == PadSetChoice(Crew);
}

FString ApexPitPlan::Summary(const FApexPitPlan& Plan, const TArray<FString>& Compounds)
{
	TArray<FString> Parts;
	const int32 Tyres = TyreChoice(Plan);
	if (Tyres == 0)
	{
		Parts.Add(TEXT("KEEP TYRES"));
	}
	else if (Tyres >= 2)
	{
		Parts.Add(TyreChoiceLabel(Tyres, Compounds));
	}
	if (FuelChoice(Plan) >= 0)
	{
		Parts.Add(FuelChoice(Plan) == 0 ? FString(TEXT("NO FUEL")) : FuelChoiceLabel(FuelChoice(Plan)));
	}
	if (Plan.bNoRepair)
	{
		Parts.Add(TEXT("NO REPAIR"));
	}
	if (const int32 Pads = PadsChoice(Plan); Pads > 0)
	{
		Parts.Add(Pads == 1 ? TEXT("PADS KEPT") : TEXT("NEW PADS"));
	}
	if (const int32 Set = PadSetChoice(Plan); Set > 0)
	{
		Parts.Add(PadSetChoiceLabel(Set) + TEXT(" PADS"));
	}
	if (Parts.Num() == 0)
	{
		Parts.Add(TEXT("CREW'S CALL"));
	}
	if (Plan.bBoxThisLap)
	{
		Parts.Insert(TEXT("BOX THIS LAP"), 0);
	}
	return FString::Join(Parts, TEXT(" · "));
}

int32 ApexPitPlan::PadSetFitted(const FApexPitPlan& Plan, float WornBeforePct, float WornAfterPct)
{
	if (!Plan.bChoosePadSet || Plan.Pads == 0)
	{
		return INDEX_NONE;
	}
	// New pads go on whatever their wear; the crew's call fits them only past
	// the change point, which the wear coming down over the stop shows.
	const bool bFitted = Plan.Pads > 0
		|| (WornBeforePct >= 0.0f && WornAfterPct >= 0.0f && WornAfterPct < WornBeforePct - 0.5f);
	return bFitted ? FMath::Clamp(Plan.PadSet, -1, 1) : INDEX_NONE;
}

// ---------------------------------------------------------------------------
// The subsystem
// ---------------------------------------------------------------------------

void UApexPitPlanSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexSettingsSubsystem>();
	if (UApexNetSubsystem* Net = Collection.InitializeDependency<UApexNetSubsystem>())
	{
		Net->OnSessionJoined.AddDynamic(this, &UApexPitPlanSubsystem::HandleSessionJoined);
		Net->OnTelemetry.AddDynamic(this, &UApexPitPlanSubsystem::HandleTelemetry);
	}
}

void UApexPitPlanSubsystem::Deinitialize()
{
	if (UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr)
	{
		Net->OnSessionJoined.RemoveAll(this);
		Net->OnTelemetry.RemoveAll(this);
	}
	Super::Deinitialize();
}

const TArray<FString>& UApexPitPlanSubsystem::GetCompounds() const
{
	static const TArray<FString> None;
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	return Net ? Net->GetCarSetupSheet().Compounds : None;
}

void UApexPitPlanSubsystem::SetPlan(const FApexPitPlan& NewPlan)
{
	Plan = NewPlan;
	Send();
}

void UApexPitPlanSubsystem::ToggleBoxThisLap()
{
	Plan.bBoxThisLap = !Plan.bBoxThisLap;
	UE_LOG(LogApexSim, Log, TEXT("Box this lap %s"), Plan.bBoxThisLap ? TEXT("requested") : TEXT("called off"));
	Send();
}

void UApexPitPlanSubsystem::Send()
{
	UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || !Net->IsInSession() || Net->IsSessionSpectator())
	{
		return;
	}
	PlanSessionId = Net->GetCurrentSessionId();
	Net->SetPitPlan(Plan);
}

void UApexPitPlanSubsystem::HandleSessionJoined(const FString& SessionId, int32 GridPosition)
{
	bWasServicing = false;
	PadWearBeforePct = -1.0f;
	LastPadWearPct = -1.0f;
	if (SessionId != PlanSessionId)
	{
		// Another session: its crew start from their own call.
		Plan = FApexPitPlan();
		PlanSessionId = SessionId;
		return;
	}
	// Back in the same session: the server has forgotten the plan.
	if (!ApexPitPlan::IsCrewsCall(Plan) || Plan.bBoxThisLap)
	{
		UE_LOG(LogApexSim, Log, TEXT("Rejoined %s: sending the pit plan again"), *SessionId);
		Send();
	}
}

void UApexPitPlanSubsystem::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	if (!Net || !Net->IsInSession() || Net->IsSessionSpectator())
	{
		return;
	}
	const int32 LocalIndex = Net->GetLocalCarIndex();
	const FApexCarTelemetry* Local = Frame.Cars.FindByPredicate(
		[LocalIndex](const FApexCarTelemetry& Car) { return Car.CarIndex == LocalIndex; });
	if (!Local)
	{
		return;
	}
	const float Wear = WorstPadWear(*Local);
	if (Local->bPitServicing && !bWasServicing)
	{
		// The stop is under way: the server's request is spent.
		Plan.bBoxThisLap = false;
		PadWearBeforePct = LastPadWearPct >= 0.0f ? LastPadWearPct : Wear;
	}
	else if (!Local->bPitServicing && bWasServicing)
	{
		const int32 PadSet = ApexPitPlan::PadSetFitted(Plan, PadWearBeforePct, Wear);
		if (PadSet != INDEX_NONE)
		{
			if (UApexSettingsSubsystem* Settings = GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>())
			{
				UE_LOG(LogApexSim, Log, TEXT("The stop fitted pad set %d: the setup's brake_pads knob follows"), PadSet);
				Settings->SetCarSetupClick(ApexCarSetup::BrakePads, PadSet);
			}
		}
	}
	bWasServicing = Local->bPitServicing;
	if (!Local->bPitServicing)
	{
		LastPadWearPct = Wear;
	}
}
