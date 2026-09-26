#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Tickable.h"
#include "Track/ApexTrackSceneData.h"
#include "UObject/Object.h"

#include "ApexTrackInstance.generated.h"

class AActor;
class FApexRuntimeTrackFactory;
class FApexTrackSceneBuilder;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UWorld;
struct FApexMaterialParams;
struct FApexTrackGeometry;

/** A runtime track the content scan found. */
struct FApexRuntimeTrackFiles
{
	FString Stem;
	/** The manifest, `<dir>/<Stem>.uescene.json`. */
	FString ScenePath;
	/** `<Stem>.png` beside it or under `previews/`; empty when there is none. */
	FString PreviewPath;
	FApexTrackSceneHeader Header;
};

/**
 * One circuit in the game world, built from its export on disk.
 *
 * There are no cooked track levels: every circuit is built here by
 * `FApexTrackSceneBuilder` from its `<Stem>.uescene.json` + `<Stem>.uemesh`.
 * The files are read and the mesh descriptions filled on a worker thread,
 * then the materials, meshes and actors are made on the game thread within
 * a time budget per frame (`apexsim.track.BuildBudgetMs`), and the track
 * counts as loaded once its collision has cooked. The race director sees
 * loaded, visible, and a list of actors with the tags the builder gives them.
 *
 * Owned by `UApexTrackContentSubsystem`, which hands them out and takes
 * them back (`Acquire` / `Release`).
 */
UCLASS(Transient)
class APEXSIM_API UApexTrackInstance : public UObject, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UApexTrackInstance();
	virtual ~UApexTrackInstance() override;

	/** Begin building the track in `Files` into `World`. False when it cannot even start. */
	bool Start(UWorld* InWorld, const FApexRuntimeTrackFiles& Files);

	const FString& GetStem() const { return Stem; }
	UWorld* GetWorld() const override;

	/** Everything is in the world: the build has finished and the collision has cooked. */
	bool IsLoaded() const;
	/** Loaded, shown and collidable: what the racing line and cameras wait for. */
	bool IsVisible() const;
	bool HasFailed() const;

	/** Show or hide the whole track (the demo behind the car-select screen). */
	void SetVisible(bool bInVisible);

	/** Take everything out of the world. The instance is spent afterwards. */
	void Unload();

	/**
	 * A runtime track handed out again: every material back to what the
	 * builder made it (the race director wets the road and lights the lamps
	 * on the shared instances) and shown.
	 */
	void ResetForReuse();

	/** The track's actors. */
	void GetActors(TArray<AActor*>& OutActors) const;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);

private:
	friend class FApexRuntimeTrackFactory;

	enum class EPhase : uint8
	{
		Idle,
		Parsing,
		Materials,
		Meshes,
		Actors,
		Cooking,
		Ready,
		Failed,
	};

	/** What the worker thread hands back. */
	struct FParsed
	{
		bool bOk = false;
		FString Error;
		double Seconds = 0.0;
		TSharedPtr<FApexTrackScene> Scene;
		TSharedPtr<FApexTrackGeometry> Geometry;
	};

	void Fail(const FString& Why);
	void ApplyVisibility();
	void ApplyMaterialParams(UMaterialInstanceDynamic* Material, const FApexMaterialParams& Params) const;

	FString Stem;
	FString ScenePath;
	TWeakObjectPtr<UWorld> World;
	EPhase Phase = EPhase::Idle;
	bool bVisible = true;
	double StartedAt = 0.0;
	double ParseSeconds = 0.0;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> Actors;

	/** Everything the runtime factory made: materials and meshes. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> Created;

	/** Engine assets the runtime build falls back to, referenced so the cook carries them. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> PlaceholderCube;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FallbackMaterial;

	/** Each dynamic instance and the parameters it was built with, for `ResetForReuse`. */
	TArray<TPair<TWeakObjectPtr<UMaterialInstanceDynamic>, TSharedPtr<FApexMaterialParams>>> MaterialDefaults;

	TFuture<TSharedPtr<FParsed>> Parsing;
	TSharedPtr<FApexTrackScene> Scene;
	TSharedPtr<FApexTrackGeometry> Geometry;
	// Shared rather than unique pointers: the types are only complete in the
	// .cpp, and a unique pointer's deleter would be needed by generated code.
	TSharedPtr<FApexRuntimeTrackFactory> Factory;
	TSharedPtr<FApexTrackSceneBuilder> Builder;
};
