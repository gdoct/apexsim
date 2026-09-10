#pragma once

#include "CoreMinimal.h"

#include "ApexCockpitLayout.generated.h"

/**
 * Whether a car is driven from an open cockpit or a closed cabin.
 *
 * The two want different seats: an open-wheeler's eye sits low and on the
 * centreline with mirrors beside the cockpit opening and no centre mirror; a
 * closed car's sits to one side, higher, with a mirror on the windscreen.
 * Auto derives it from the catalog's class and, failing that, the height of
 * the mesh.
 */
UENUM(BlueprintType)
enum class EApexCockpitStyle : uint8
{
	Auto,
	OpenWheel,
	Closed,
};

/**
 * Per-car hand-placed cockpit points, in the car's own frame (cm, +X nose,
 * +Y right, +Z up, origin the actor's). Zero means "derive from the mesh".
 *
 * Lives on the car catalog row so a car whose mesh has a modelled interior
 * can put the eye exactly in its seat and the mirrors on its own housings.
 */
USTRUCT(BlueprintType)
struct APEXSIM_API FApexCockpitOverrides
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cockpit")
	EApexCockpitStyle Style = EApexCockpitStyle::Auto;

	/** Driver's eye. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cockpit")
	FVector Eye = FVector::ZeroVector;

	/** Centre of the steering wheel hub. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cockpit")
	FVector Wheel = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cockpit")
	FVector MirrorCentre = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cockpit")
	FVector MirrorLeft = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cockpit")
	FVector MirrorRight = FVector::ZeroVector;
};

/**
 * Where everything in the cockpit sits, in the car's frame (cm).
 *
 * Derived once per car from its mesh bounds (see ApexCockpit::DeriveLayout);
 * the player's seat adjustments are applied on top by the director, not
 * baked in here, so a change in the settings overlay moves the eye without
 * touching the wheel or the mirrors.
 */
struct APEXSIM_API FApexCockpitLayout
{
	bool bOpenWheel = false;

	/** Driver's eye with the seat in its neutral position. */
	FVector Eye = FVector::ZeroVector;

	/** Centre of the wheel hub, where the display sits. */
	FVector Wheel = FVector::ZeroVector;
	/** Pitch of the wheel plane: positive tips the top of the rim toward the driver. */
	float WheelRakeDeg = 20.0f;
	/** Rim rotation at full steering input, one way. */
	float WheelLockDeg = 120.0f;
	/** Half the rim's width; grips sit at ±this. */
	float WheelHalfWidthCm = 14.0f;

	bool bCentreMirror = false;
	FVector MirrorCentre = FVector::ZeroVector;
	FVector MirrorLeft = FVector::ZeroVector;
	FVector MirrorRight = FVector::ZeroVector;
	/** Glass size, width × height. */
	FVector2D CentreMirrorSizeCm = FVector2D(26.0f, 8.0f);
	FVector2D SideMirrorSizeCm = FVector2D(16.0f, 10.0f);

	/** Top of the bodywork on the centreline; the virtual mirror looks back from above it. */
	float RoofZ = 0.0f;
};

namespace ApexCockpit
{
	/**
	 * A mesh's local bounds carried into the car actor's frame.
	 *
	 * The imported meshes have their long axis on Y and are mounted with a
	 * -90° yaw so the nose points +X; the box has to go through the same
	 * transform or the eye ends up beside the car instead of inside it.
	 */
	APEXSIM_API FBox ActorFrameBox(const FBoxSphereBounds& MeshLocalBounds, const FTransform& MeshRelativeTransform);

	/** A plausible GT-sized box for a car whose mesh has not loaded. */
	APEXSIM_API FBox FallbackBox();

	/**
	 * Auto resolves from the catalog class ("F1" and anything naming an open
	 * cockpit) and then from proportions: nothing closed is under 1.1 m tall.
	 */
	APEXSIM_API EApexCockpitStyle ResolveStyle(EApexCockpitStyle Requested, const FString& CarClass, const FBox& Box);

	/**
	 * Seat, wheel and mirrors from the bodywork's box, with any non-zero
	 * override taking the place of the derived point.
	 *
	 * The proportions are those of the real things: a GT driver's eye about
	 * 70% of the way up the roofline and a fifth of the width left of centre,
	 * an open-wheeler's lower and on the centreline; the wheel a forearm ahead
	 * and below; door mirrors just outside the body.
	 */
	APEXSIM_API FApexCockpitLayout DeriveLayout(const FBox& Box, EApexCockpitStyle Style, const FApexCockpitOverrides& Overrides);

	/**
	 * The camera's world rotation from the car's.
	 *
	 * HorizonLock 0 rides the car — through a banked corner the horizon tilts,
	 * over a kerb it jolts — and 1 keeps pitch and roll level as an inner ear
	 * would, leaving only yaw. ViewPitch is the driver's chosen gaze and
	 * LookYaw the head turn; both are in the car's frame, so looking back
	 * through a roll still rolls with the car.
	 */
	APEXSIM_API FQuat ViewRotation(const FRotator& CarRotation, float HorizonLock, float ViewPitchDeg, float LookYawDeg);

	/**
	 * Where inertia moves the head, cm in the car's frame.
	 *
	 * Braking throws the head forward, a right-hander throws it left; both
	 * are a few centimetres at 1 g and clamp before they read as the camera
	 * leaving the seat. Amount is the player's 0..1 head-motion setting.
	 */
	APEXSIM_API FVector HeadLean(float LateralG, float LongitudinalG, float Amount);

	/**
	 * Rim roll for a steering input.
	 *
	 * The server's steering is positive to the left. In Unreal positive roll
	 * about the wheel's forward axis is clockwise as the driver sees it, and
	 * turning left is counter-clockwise, hence the sign.
	 */
	inline float WheelRollDeg(float ServerSteering, float LockDeg)
	{
		return -FMath::Clamp(ServerSteering, -1.0f, 1.0f) * LockDeg;
	}

	/**
	 * Yaw the head turns toward the apex for a steering input, degrees.
	 *
	 * Same sign convention as the rim: left steering is a negative Unreal
	 * yaw. Capped well below a real head turn so it reads as anticipation,
	 * not as looking out of the side window.
	 */
	inline float ApexLookYawDeg(float ServerSteering, float Amount)
	{
		return -FMath::Clamp(ServerSteering, -1.0f, 1.0f) * 14.0f * FMath::Clamp(Amount, 0.0f, 1.0f);
	}
}
