#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Race/ApexCarDamage.h"

#include "ApexCarEffectsActor.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class AApexCarDebrisActor;

/**
 * The race's smoke, steam and sparks, for every car at once.
 *
 * There are no particle assets in the project (the rain is an instanced
 * mesh too), so a puff is an instance of the engine's sphere and a spark
 * one of its cube, both drawn with `M_ApexCarSmoke` (a lit translucent
 * parent baked beside the car parents) whose per-instance data carries the
 * puff's opacity, shade, glow and edge softness. The puffs live in two
 * fixed rings (the oldest is reused when a ring is full) and move in world
 * space by `ApexDamage::StepPuff`; the cars emit into them
 * (`AApexRaceCarActor`). One per world, made on first use (`Get`); it also
 * keeps the race's debris to a bounded number.
 */
UCLASS()
class APEXSIM_API AApexCarEffectsActor : public AActor
{
	GENERATED_BODY()

public:
	AApexCarEffectsActor();

	/**
	 * The world's effects actor, spawned the first time it is asked for
	 * (unless `bSpawn` is false); null without a game world.
	 */
	static AApexCarEffectsActor* Get(UWorld* World, bool bSpawn = true);

	virtual void Tick(float DeltaSeconds) override;

	/** Starts a puff (smoke, steam, dust) or a spark (`Puff.bSpark`). Nothing without the material. */
	void Emit(const ApexDamage::FPuff& Puff);

	/**
	 * Keeps a piece of debris on the books: past `MaxDebris` the oldest is
	 * destroyed, so a long race of shunts does not fill the track.
	 */
	void AddDebris(AApexCarDebrisActor* Debris);

	/** Puts out every puff and clears every piece of debris (a new race). */
	void Clear();

	/** Whether the smoke material was baked; without it nothing is drawn. */
	bool CanDraw() const { return bHaveMaterial; }

	static constexpr int32 MaxSmoke = 480;
	static constexpr int32 MaxSparks = 320;
	static constexpr int32 MaxDebris = 40;

private:
	void EnsureInstances();
	/** Writes one ring's instances; returns whether any puff in it is alive. */
	bool WriteRing(UInstancedStaticMeshComponent& Component, TArray<ApexDamage::FPuff>& Ring, TArray<FTransform>& Transforms,
		TArray<float>& Data, float DeltaSeconds, const FVector& CameraLocation);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> Smoke;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> Sparks;

	TArray<ApexDamage::FPuff> SmokeRing;
	TArray<ApexDamage::FPuff> SparkRing;
	TArray<FTransform> SmokeTransforms;
	TArray<FTransform> SparkTransforms;
	TArray<float> SmokeData;
	TArray<float> SparkData;
	int32 NextSmoke = 0;
	int32 NextSpark = 0;
	/** A ring that had live puffs last frame is written once more to hide them. */
	bool bSmokeLive = false;
	bool bSparksLive = false;
	bool bHaveMaterial = false;
	bool bInstancesMade = false;

	TArray<TWeakObjectPtr<AApexCarDebrisActor>> Debris;
};

/**
 * A piece of bodywork thrown off a car (FApexDamagePartSpec): a copy of the
 * part's mesh, materials (livery included) and dents, flying from where it
 * was by `ApexDamage::StepDebris` and landing on whatever the track's
 * collision says is under it. Cosmetic: the server knows nothing of it and
 * the cars drive through it.
 */
UCLASS()
class APEXSIM_API AApexCarDebrisActor : public AActor
{
	GENERATED_BODY()

public:
	AApexCarDebrisActor();

	/**
	 * Copies `Source` (a part's component on a car) and sets it flying at
	 * `Velocity` (cm/s) spinning at `Spin` (rad/s). `FallbackGroundZ` is the
	 * ground when no trace finds the track under it.
	 */
	void Launch(const UStaticMeshComponent& Source, const FVector& Velocity, const FVector& Spin, float FallbackGroundZ);

	virtual void Tick(float DeltaSeconds) override;

	/** How long a piece lies on the track before it is cleared. */
	static constexpr float LifeSeconds = 40.0f;

private:
	/** The ground under `At`, from the track's collision, or the fallback. */
	float GroundUnder(const FVector& At) const;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Mesh;

	ApexDamage::FDebris Body;
	float FallbackGround = 0.0f;
	/** How far its middle sits above the ground when it lies flat, cm. */
	float RestHeight = 0.0f;
	float Age = 0.0f;
	float NextTrace = 0.0f;
};
