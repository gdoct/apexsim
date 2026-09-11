#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexSettingsSave.h"
#include "GameFramework/Actor.h"

#include "ApexRacingLineActor.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

/**
 * The racing line painted on the road as a dotted line: green where the car
 * is flat out, amber where it is held at the grip limit or lifting, red
 * where it brakes.
 *
 * The line and its phases come from the server (`RacingLine`, built for the
 * local car), so there is nothing to compute here but where each dot sits.
 * Dots are flat discs, one instanced mesh per colour, so a whole circuit is
 * three draw calls. They are dropped onto the rendered road with line traces
 * once the track level is in the world; until then they sit at the line's
 * own heights, which are the track file's and can be a little off the baked
 * road.
 *
 * Spawned by the race director for the length of a race.
 */
UCLASS()
class APEXSIM_API AApexRacingLineActor : public AActor
{
	GENERATED_BODY()

public:
	AApexRacingLineActor();

	/** Replace the line. Dots are laid at the line's own heights until SnapToGround. */
	void SetLine(const FApexRacingLineData& InLine);

	/**
	 * Re-lay the dots on the road of `TrackLevel`, found by tracing down at each
	 * one. Only the level's own track meshes count as ground: not props, and not
	 * whatever the menu world has lying about near the origin.
	 */
	void SnapToGround(const ULevel* TrackLevel);

	/** Which colours are drawn: none, only the braking zones, or all of it. */
	void SetMode(EApexRacingLine InMode);

	bool HasLine() const { return Line.IsValid(); }
	bool IsOnGround() const { return bOnGround; }

protected:
	virtual void BeginPlay() override;

private:
	UInstancedStaticMeshComponent* MakeDots(const TCHAR* Name);
	void Paint(UInstancedStaticMeshComponent* Dots, const FLinearColor& Color);

	/** Lay every dot, tracing down onto `Ground` when it is given. */
	void Rebuild(const ULevel* Ground);
	void ApplyVisibility();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> ThrottleDots;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> PartialDots;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> BrakeDots;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> DotMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ShapeMaterial;

	FApexRacingLineData Line;
	EApexRacingLine Mode = EApexRacingLine::Off;
	bool bOnGround = false;
};
