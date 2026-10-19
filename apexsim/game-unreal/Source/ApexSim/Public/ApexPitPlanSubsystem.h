#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexPitPlanSubsystem.generated.h"

/**
 * The driver's pit plan as the pause menu's panel shows it: each order a
 * stepper of choices, read and written here, pure for the tests
 * (`ApexSim.Race.PitPlan`).
 */
namespace ApexPitPlan
{
	/** Litres per step of the fuel row. */
	inline constexpr float FuelStepL = 5.0f;
	/** The fuel row's last step (150 L): the server caps it by the tank. */
	inline constexpr int32 FuelMaxSteps = 30;

	/** The tyres row: 0 keep, 1 the setup's next compound, 2.. the car's compound (index - 2). */
	APEXSIM_API int32 TyreChoice(const FApexPitPlan& Plan);
	APEXSIM_API void SetTyreChoice(FApexPitPlan& Plan, int32 Choice);
	/** KEEP, SETUP'S, then the compound's name (SOFT, MEDIUM, HARD for a car that names none). */
	APEXSIM_API FString TyreChoiceLabel(int32 Choice, const TArray<FString>& Compounds);
	/** How many choices the tyres row has for a car with these compounds (the three defaults for none). */
	APEXSIM_API int32 TyreChoiceCount(const TArray<FString>& Compounds);

	/** The fuel row: -1 to need, 0 none, N steps of FuelStepL. */
	APEXSIM_API int32 FuelChoice(const FApexPitPlan& Plan);
	APEXSIM_API void SetFuelChoice(FApexPitPlan& Plan, int32 Choice);
	/** TO THE END, NONE, +5 L. */
	APEXSIM_API FString FuelChoiceLabel(int32 Choice);

	/** The repairs row: 0 repair, 1 leave the damage. */
	APEXSIM_API FString RepairChoiceLabel(int32 Choice);

	/** The pads row: 0 the crew's call (past the change point), 1 keep, 2 new. */
	APEXSIM_API int32 PadsChoice(const FApexPitPlan& Plan);
	APEXSIM_API void SetPadsChoice(FApexPitPlan& Plan, int32 Choice);
	APEXSIM_API FString PadsChoiceLabel(int32 Choice);

	/** The pad set row: 0 the setup's set, 1 endurance, 2 standard, 3 sprint. */
	APEXSIM_API int32 PadSetChoice(const FApexPitPlan& Plan);
	APEXSIM_API void SetPadSetChoice(FApexPitPlan& Plan, int32 Choice);
	APEXSIM_API FString PadSetChoiceLabel(int32 Choice);

	/** Every order at the crew's own choice (the box request aside): nothing worth sending. */
	APEXSIM_API bool IsCrewsCall(const FApexPitPlan& Plan);

	/** The plan in a line for the pause menu's row: "CREW'S CALL", "KEEP TYRES · +5 L · NO REPAIR". */
	APEXSIM_API FString Summary(const FApexPitPlan& Plan, const TArray<FString>& Compounds);

	/**
	 * The pad set the setup's `brake_pads` knob holds after a stop made on
	 * this plan, or INDEX_NONE when it keeps its own: only a plan that chose
	 * a set changes it, and only when pads went on (always for new pads; for
	 * the crew's call, when the worst pad's wear went down over the stop).
	 * Wear is percent, negative when unknown.
	 */
	APEXSIM_API int32 PadSetFitted(const FApexPitPlan& Plan, float WornBeforePct, float WornAfterPct);
}

/**
 * The driver's orders for their next stop (`SetPitPlan`) and the box-this-lap
 * request, kept for the session they were given in.
 *
 * The pause menu's pit plan panel edits it and the `BoxThisLap` key toggles
 * the request; every change is sent at once. The server keeps the last plan,
 * but not across a rejoin, so joining the same session again sends it again;
 * joining another session starts from the crew's call. Watching the local
 * car's telemetry it drops the request once the stop starts (the server has
 * then cleared its own) and, after a stop on a plan that fitted another pad
 * set, moves the garage setup's `brake_pads` knob to it, as the server moved
 * its own: otherwise the next `SetCarSetup` would put the old set back.
 */
UCLASS()
class APEXSIM_API UApexPitPlanSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	const FApexPitPlan& GetPlan() const { return Plan; }

	/** Keeps the plan and sends it, if the player is in a session. */
	void SetPlan(const FApexPitPlan& NewPlan);

	/** The box-this-lap request on or off, and the plan sent with it. */
	void ToggleBoxThisLap();

	/** The local car's compound names, from the setup sheet; empty for the defaults. */
	const TArray<FString>& GetCompounds() const;

private:
	UFUNCTION()
	void HandleSessionJoined(const FString& SessionId, int32 GridPosition);

	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);

	void Send();

	FApexPitPlan Plan;
	/** The session the plan was given in. */
	FString PlanSessionId;

	/** The local car was being serviced on the last frame. */
	bool bWasServicing = false;
	/** The worst pad's wear before the stop under way, percent; negative when unknown. */
	float PadWearBeforePct = -1.0f;
	float LastPadWearPct = -1.0f;
};
