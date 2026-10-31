#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ApexGrassMarks.generated.h"

class AApexRaceCarActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UTexture2D;

/**
 * Tyre marks in the grass (docs/game/client.md, "Tyre marks in the grass"):
 * a car that runs wide leaves the flattened, darker grass of its tyres in
 * the verge, and torn earth where it slid or braked, kept for the session.
 * The server's ground under a tyre names gravel and sand only (grass reads
 * as the road), so the client finds grass itself: a trace under each wheel's
 * contact patch onto a component the scene builder tagged (`GrassTag`, the
 * terrain and the grass bands of a circuit whose ground is grass). The
 * maths here is pure, for tests; `AApexGrassMarksActor` traces and draws.
 */
namespace ApexGrassMarks
{
	/** A mark is laid each time a wheel has run this far on grass, cm. */
	inline constexpr float StepCm = 70.0f;
	/** A wheel that comes back further than this from its last mark starts a new trail, cm. */
	inline constexpr float BreakCm = 400.0f;
	/** A tyre's print, cm. */
	inline constexpr float WidthCm = 28.0f;
	/** Marks kept for the session; past it the oldest go first. */
	inline constexpr int32 MaxMarks = 30000;
	/** A car sliding past this angle, deg, tears the turf to earth (a GT3 takes a hairpin near 9). */
	inline constexpr float EarthSlipDeg = 14.0f;
	/** Or braking harder than this, m/s². */
	inline constexpr float EarthDecelMps2 = 8.0f;
	/** Ground steeper than this (its normal's Z under it) is a bank's face, not a verge: no mark. */
	inline constexpr float MinGroundUp = 0.82f;
	/** Nothing is laid under this speed, m/s. */
	inline constexpr float MinSpeedMps = 3.0f;
	/** How far over the ground a mark floats, cm, clear of the terrain's own triangles. */
	inline constexpr float LiftCm = 3.0f;

	/** One mark: a segment of a tyre's print on the ground. */
	struct FMark
	{
		FVector From = FVector::ZeroVector;
		FVector To = FVector::ZeroVector;
		/** The ground's normal under it. */
		FVector Normal = FVector::UpVector;
		/** Torn earth rather than flattened grass. */
		bool bEarth = false;
	};

	/** One wheel's trail: where its last mark ended. */
	struct FTrail
	{
		FVector Last = FVector::ZeroVector;
		bool bOpen = false;
	};

	/**
	 * Feed one wheel's ground point. Off the grass the trail closes; on it,
	 * the first point opens it and each `StepCm` of travel lays a mark from
	 * the last one (true, `Out` filled); a jump past `BreakCm` opens it again
	 * without a mark across the gap.
	 */
	APEXSIM_API bool Advance(FTrail& Trail, const FVector& Point, const FVector& Normal, bool bOnGrass, bool bEarth, FMark& Out);

	/**
	 * The instance transform that lays the engine's 1 m plane (+Z up) along
	 * a mark: centred on it, its X along the mark, its Z the ground's
	 * normal, `WidthCm` across, lifted `LiftCm`, a little longer than the
	 * mark so neighbours meet.
	 */
	APEXSIM_API FTransform MarkTransform(const FMark& Mark, float Width = WidthCm);

	/** A car's slip angle, deg: between where it points and where it goes; 0 when slow. */
	APEXSIM_API float SlipDeg(const FVector& Velocity, const FVector& Forward);

	/** Whether a car going at these figures tears the turf (`EarthSlipDeg`, `EarthDecelMps2`). */
	APEXSIM_API bool TearsEarth(float SlipAngleDeg, float DecelMps2);

	/** The component tag the scene builder sets on the surfaces that take marks. */
	APEXSIM_API FName GrassTag();

	/**
	 * The mark's texture, `Size` across a print by `Size * 4` along it:
	 * grey streaks of laid blades down its length (70-100 %), its alpha the
	 * print with ragged edges, tread bars and gaps, so
	 * the masked decal parent cuts a patchy track rather than a painted band.
	 * BGRA8, row by row.
	 */
	APEXSIM_API TArray<FColor> MarkPixels(int32 Size);
}

/**
 * Draws the session's tyre marks in the grass: two instanced planes on the
 * masked decal parent (`M_ApexDecal`), flattened grass and torn earth, a
 * trail per car and wheel. The race director calls `Sample` every frame;
 * it traces a few times a second.
 */
UCLASS(NotPlaceable)
class APEXSIM_API AApexGrassMarksActor : public AActor
{
	GENERATED_BODY()

public:
	AApexGrassMarksActor();

	/** Trace under every car's wheels (a few times a second) and lay what they ran over. */
	void Sample(const TMap<int32, TObjectPtr<AApexRaceCarActor>>& Cars, float DeltaSeconds);

	/** Forget every mark and trail (another track, another session). */
	void Clear();

	int32 NumMarks() const;

	/** The newest mark, false before any: where `apexsim.road.GrassMarksLook` points the shot camera. */
	bool GetNewestMark(ApexGrassMarks::FMark& Out) const;

private:
	void EnsureComponents();
	void Lay(const ApexGrassMarks::FMark& Mark);

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Grass;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Earth;
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> MarkTexture;

	/** A trail per car and wheel: the key is car index x 4 + wheel. */
	TMap<int32, ApexGrassMarks::FTrail> Trails;
	/** Each car's speed at the last pass, m/s, for its braking. */
	TMap<int32, float> PrevSpeed;
	float SinceSample = 0.0f;
	bool bComponentsTried = false;
	ApexGrassMarks::FMark Newest;
	bool bHasNewest = false;
	/** The count last logged, in hundreds. */
	int32 LoggedHundreds = 0;
};
