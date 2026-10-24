#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "School/ApexSchool.h"

#include "ApexSchoolMarkerActor.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

/**
 * A licence test's marks painted across the road (the driving school,
 * School/ApexSchool.h): the brake test's board in yellow, its stop line in
 * white and the gold box either side of it in green; a corner test's entry
 * and exit gates in white. Flat strips, one instanced mesh per colour, laid
 * at the test's stations on the track's centerline and dropped onto the
 * road by line traces, as the racing line's dots are. Spawned by
 * UApexSchoolSubsystem for the school's session.
 */
UCLASS()
class APEXSIM_API AApexSchoolMarkerActor : public AActor
{
	GENERATED_BODY()

public:
	AApexSchoolMarkerActor();

	/** Lay the marks of `Test` on the road; `CenterlineCm` is the track's as built. */
	void SetMarks(const ApexSchool::FTest& Test, const TArray<FVector>& CenterlineCm);

protected:
	virtual void BeginPlay() override;

private:
	UInstancedStaticMeshComponent* MakeStrips(const TCHAR* Name);
	void Paint(UInstancedStaticMeshComponent* Strips, const FLinearColor& Color);
	/** One strip across the road at a station, LengthM along it. */
	void AddStrip(UInstancedStaticMeshComponent* Strips, const TArray<FVector>& CenterlineCm, float StationM, float LengthM);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> BoardStrips;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> LineStrips;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> BoxStrips;

	UPROPERTY()
	TObjectPtr<UStaticMesh> StripMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> ShapeMaterial;
};
