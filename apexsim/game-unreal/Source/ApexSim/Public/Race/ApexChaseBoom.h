#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpringArmComponent.h"

#include "ApexChaseBoom.generated.h"

/**
 * The chase camera's spring arm, kept out of the followed car's bodywork.
 *
 * The boom has no collision test (the track's props would pull it in), so
 * nothing stopped its lag from carrying the camera through the car itself
 * when the car spun or stopped dead. After the arm has placed the camera,
 * `ApexChase::KeepOutOfBody` lifts it clear of the body box the director
 * hands in each frame.
 */
UCLASS()
class APEXSIM_API UApexChaseBoom : public USpringArmComponent
{
	GENERATED_BODY()

public:
	/** The followed car's transform and its body box in that frame. */
	void SetBody(const FTransform& InBodyToWorld, const FBox& InBodyBox);

	/** No car to keep out of. */
	void ClearBody();

protected:
	virtual void UpdateDesiredArmLocation(bool bDoTrace, bool bDoLocationLag, bool bDoRotationLag, float DeltaTime) override;

private:
	FTransform BodyToWorld = FTransform::Identity;
	FBox BodyBox = FBox(ForceInit);
};
