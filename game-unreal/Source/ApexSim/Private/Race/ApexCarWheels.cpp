#include "Race/ApexCarWheels.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

namespace ApexWheels
{
	float SteerAngleRad(const FApexWheelSpec& Spec, float SteeringInput)
	{
		return FMath::Clamp(SteeringInput, -1.0f, 1.0f) * FMath::Max(Spec.MaxSteerRad, 0.0f);
	}

	float RollAngleRad(float DistanceM, float RadiusM)
	{
		return RadiusM > KINDA_SMALL_NUMBER ? DistanceM / RadiusM : 0.0f;
	}

	float DrawnSpinStepRad(float StepRad, float MaxStepRad)
	{
		return MaxStepRad > 0.0f ? FMath::Clamp(StepRad, -MaxStepRad, MaxStepRad) : StepRad;
	}

	FTransform WheelTransform(
		const FApexWheelSpec& Spec, EWheel Wheel, const FBoxSphereBounds& WheelMeshBounds, float SteerRad, float SpinRad)
	{
		const bool bFront = IsFront(Wheel);
		const bool bLeft = IsLeft(Wheel);
		const float Axle = bFront ? Spec.FrontAxleM : Spec.RearAxleM;
		const float Track = bFront ? Spec.FrontTrackM : Spec.RearTrackM;
		const float Radius = bFront ? Spec.FrontRadiusM : Spec.RearRadiusM;
		const float Width = bFront ? Spec.FrontWidthM : Spec.RearWidthM;

		// Body frame: left is +X, the nose +Y.
		const FVector Hub((bLeft ? 0.5f : -0.5f) * Track * 100.0f, Axle * 100.0f, Radius * 100.0f);

		// The wheel's face is on its +X: left as it comes, turned about Z for
		// the right side so the face is outboard there too. A turn, not a
		// mirror, so the model is never drawn inside out.
		const FQuat Base = bLeft ? FQuat::Identity : FQuat(FVector::ZAxisVector, UE_PI);

		// Rolling forward (+Y) turns a wheel about the body's -X: the top
		// moves to +Y, since (-X) x (+Z) = +Y. On the left that is the
		// wheel's own -X; the right-hand wheel is turned round, so its +X.
		const FQuat Spin(FVector::XAxisVector, bLeft ? -SpinRad : SpinRad);

		// A left turn swings the front of the wheel from +Y toward +X (left).
		// A positive rotation about Z carries +Y toward -X, since
		// (+Z) x (+Y) = -X, so a left turn is a negative one — the same sign
		// as Unreal's yaw for a left turn in the actor's own frame.
		const FQuat Steer = bFront ? FQuat(FVector::ZAxisVector, -SteerRad) : FQuat::Identity;

		const FVector Extent = WheelMeshBounds.BoxExtent;
		const FVector Scale(
			Extent.X > KINDA_SMALL_NUMBER ? Width * 50.0f / Extent.X : 1.0f,
			Extent.Y > KINDA_SMALL_NUMBER ? Radius * 100.0f / Extent.Y : 1.0f,
			Extent.Z > KINDA_SMALL_NUMBER ? Radius * 100.0f / Extent.Z : 1.0f);

		// Scale in the wheel's own frame (before any turn), so it stays
		// width along the axle whatever the spin and steer.
		return FTransform(Steer * Base * Spin, Hub, Scale);
	}

	FBox WheelsBox(const FApexWheelSpec& Spec)
	{
		FBox Box(ForceInit);
		if (!Spec.IsUsable())
		{
			return Box;
		}
		const FBoxSphereBounds Unit(FVector::ZeroVector, FVector(50.0f), 87.0f);
		for (int32 i = 0; i < NumWheels; ++i)
		{
			const FTransform T = WheelTransform(Spec, static_cast<EWheel>(i), Unit, 0.0f, 0.0f);
			Box += Unit.GetBox().TransformBy(T);
		}
		return Box;
	}
}

void FApexCarWheelSet::CreateComponents(UObject& Owner, USceneComponent* Body)
{
	static const TCHAR* Names[ApexWheels::NumWheels] = {
		TEXT("WheelFrontLeft"), TEXT("WheelFrontRight"), TEXT("WheelRearLeft"), TEXT("WheelRearRight")};
	Components.Reset();
	for (const TCHAR* Name : Names)
	{
		UStaticMeshComponent* Wheel = Owner.CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Wheel->SetupAttachment(Body);
		Wheel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Wheel->SetMobility(EComponentMobility::Movable);
		// Hidden until a car says which wheels it has.
		Wheel->SetVisibility(false);
		Components.Add(Wheel);
	}
}

void FApexCarWheelSet::SetSpec(const FApexWheelSpec& InSpec)
{
	Spec = InSpec;
	UStaticMesh* Loaded = Spec.IsUsable() ? Spec.Mesh.LoadSynchronous() : nullptr;
	bHasWheels = Loaded != nullptr;
	MeshBounds = Loaded ? Loaded->GetBounds() : FBoxSphereBounds(ForceInit);
	SpinRad[0] = SpinRad[1] = 0.0f;
	SteerRad = 0.0f;
	for (UStaticMeshComponent* Wheel : Components)
	{
		if (Wheel)
		{
			Wheel->SetStaticMesh(Loaded);
		}
	}
	SetVisible(bVisible);
	Place();
}

void FApexCarWheelSet::Update(float SteeringInput, float SignedSpeedMps, float DeltaSeconds, float MaxStepRad)
{
	if (!bHasWheels)
	{
		return;
	}
	const float Distance = SignedSpeedMps * FMath::Max(DeltaSeconds, 0.0f);
	const float FrontStep = ApexWheels::DrawnSpinStepRad(ApexWheels::RollAngleRad(Distance, Spec.FrontRadiusM), MaxStepRad);
	const float RearStep = ApexWheels::DrawnSpinStepRad(ApexWheels::RollAngleRad(Distance, Spec.RearRadiusM), MaxStepRad);
	SpinRad[0] = FMath::Fmod(SpinRad[0] + FrontStep, UE_TWO_PI);
	SpinRad[1] = FMath::Fmod(SpinRad[1] + RearStep, UE_TWO_PI);
	SteerRad = ApexWheels::SteerAngleRad(Spec, SteeringInput);
	Place();
}

void FApexCarWheelSet::Place()
{
	if (!bHasWheels)
	{
		return;
	}
	for (int32 i = 0; i < Components.Num() && i < ApexWheels::NumWheels; ++i)
	{
		const ApexWheels::EWheel Wheel = static_cast<ApexWheels::EWheel>(i);
		if (UStaticMeshComponent* Component = Components[i])
		{
			Component->SetRelativeTransform(ApexWheels::WheelTransform(
				Spec, Wheel, MeshBounds, SteerRad, SpinRad[ApexWheels::IsFront(Wheel) ? 0 : 1]));
		}
	}
}

void FApexCarWheelSet::SetVisible(bool bInVisible)
{
	bVisible = bInVisible;
	for (UStaticMeshComponent* Wheel : Components)
	{
		if (Wheel)
		{
			Wheel->SetVisibility(bVisible && bHasWheels);
		}
	}
}

void FApexCarWheelSet::ForEachComponent(TFunctionRef<void(UStaticMeshComponent&)> Fn) const
{
	for (UStaticMeshComponent* Wheel : Components)
	{
		if (Wheel)
		{
			Fn(*Wheel);
		}
	}
}
