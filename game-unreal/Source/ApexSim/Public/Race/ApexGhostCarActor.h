#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Race/ApexRaceCarActor.h"

#include "ApexGhostCarActor.generated.h"

/**
 * The ghost: the local driver's record lap driven by a puppet of their own
 * car, from the trace the server keeps with the record (`FApexGhostLap`).
 *
 * A race car actor rather than a new one, so it gets the catalog mesh, the
 * wheels, the brake lights and the cockpit layout for free — the replay's
 * onboard shot sits in it like in any other car. What differs is where the
 * pose comes from: not the telemetry stream but a clock, set by whoever
 * drives the ghost (the race director), read against the lap's samples.
 * In a hotlap that clock is the local car's own lap time, so the ghost is
 * exactly where the record lap was at this point of the lap; in a replay
 * it is real time from the line.
 *
 * The car is drawn opaque with a cold tint and glow — the imported glTF
 * materials cannot be made translucent at runtime — and hidden while it
 * overlaps the player's car, so it never sits inside the cockpit.
 */
UCLASS()
class APEXSIM_API AApexGhostCarActor : public AApexRaceCarActor
{
	GENERATED_BODY()

public:
	AApexGhostCarActor();

	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;

	/** The lap to drive. The ghost hides until a clock puts it on the lap. */
	void SetLap(const FApexGhostLap& InLap);
	bool HasLap() const { return Lap.IsValid(); }
	int32 GetLapTimeMs() const { return Lap.LapTimeMs; }

	/**
	 * Where on the lap the ghost is, in milliseconds from the line. Negative
	 * hides it; past the end of the lap hides it too, so a ghost that has
	 * finished does not wait at the line for the player to come round.
	 */
	void SetClockMs(float Ms);
	float GetClockMs() const { return ClockMs; }

	/** Show or hide regardless of the clock; a hidden ghost keeps its clock. */
	void SetGhostVisible(bool bVisible);
	bool IsGhostShown() const { return bShown; }

	/** Tints every material on the body and wheels; call after SetCarMesh / SetWheels. */
	void ApplyGhostLook();

	/** The pose the lap has at the clock, in the server frame; false when off the lap. */
	bool GetCurrentSample(FApexGhostSample& Out) const;

private:
	void Show(bool bVisible);

	FApexGhostLap Lap;
	float ClockMs = -1.0f;
	bool bVisibleWanted = true;
	bool bShown = false;
	bool bHasPrevLocation = false;
	FVector PrevLocation = FVector::ZeroVector;
};
