#include "Race/ApexCarWheels.h"

#include "Cars/ApexCarContentSubsystem.h"
#include "Cars/ApexCarMaterials.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

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

	float RolledDistanceM(const FVector& FromCm, const FVector& ToCm, const FQuat& Rotation, float TeleportCm)
	{
		const FVector Delta = ToCm - FromCm;
		if (Delta.Size2D() > TeleportCm)
		{
			return 0.0f;
		}
		const FVector Forward = Rotation.GetForwardVector().GetSafeNormal2D();
		return static_cast<float>(FVector::DotProduct(FVector(Delta.X, Delta.Y, 0.0), Forward) / 100.0);
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

bool ApexWheels::TreadedTint(EApexCompoundKind Kind, FLinearColor& OutBaseColour, float& OutRoughness)
{
	// The slick is authored at (0.022, 0.022, 0.024), roughness 0.8: these
	// sit a little lighter and bluer, and matte, the way a rain tyre's softer
	// compound reads next to a slick's sheen.
	switch (Kind)
	{
	case EApexCompoundKind::Intermediate:
		OutBaseColour = FLinearColor(0.030f, 0.034f, 0.044f, 1.0f);
		OutRoughness = 0.95f;
		return true;
	case EApexCompoundKind::Wet:
		OutBaseColour = FLinearColor(0.018f, 0.022f, 0.036f, 1.0f);
		OutRoughness = 0.95f;
		return true;
	default:
		return false;
	}
}

bool ApexWheels::CompoundBandColour(EApexCompoundKind Kind, FLinearColor& OutColour)
{
	switch (Kind)
	{
	case EApexCompoundKind::Intermediate:
		OutColour = FLinearColor(0.02f, 0.45f, 0.06f, 1.0f);
		return true;
	case EApexCompoundKind::Wet:
		OutColour = FLinearColor(0.02f, 0.15f, 0.80f, 1.0f);
		return true;
	default:
		OutColour = FLinearColor::Black;
		return false;
	}
}

float ApexWheels::TreadIndex(EApexCompoundKind Kind)
{
	switch (Kind)
	{
	case EApexCompoundKind::Intermediate: return 1.0f;
	case EApexCompoundKind::Wet: return 2.0f;
	default: return 0.0f;
	}
}

float ApexWheels::DeflatedShare(float PressureKpa)
{
	if (PressureKpa < 0.0f)
	{
		return 0.0f;
	}
	// Round down to 100 kPa, flat at the server's PUNCTURED_KPA (15).
	const float T = FMath::Clamp((100.0f - PressureKpa) / (100.0f - 15.0f), 0.0f, 1.0f);
	return T * T * (3.0f - 2.0f * T);
}

float ApexWheels::WetnessForWater(float WaterPct)
{
	return FMath::Clamp(WaterPct / 40.0f, 0.0f, 1.0f);
}

float ApexWheels::WetnessStep(float Current, float Target, float DeltaSeconds)
{
	if (DeltaSeconds <= 0.0f)
	{
		return Current;
	}
	const float TimeConstantS = Target > Current ? 1.5f : 20.0f;
	return Current + (Target - Current) * (1.0f - FMath::Exp(-DeltaSeconds / TimeConstantS));
}

float ApexWheels::ContactTheta(const FTransform& WheelWorld)
{
	const FVector Down = WheelWorld.InverseTransformVectorNoScale(FVector::DownVector);
	return static_cast<float>(FMath::Atan2(Down.Z, Down.Y));
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
	UStaticMesh* Loaded = Spec.IsUsable() ? ApexCarContent::LoadMesh(Spec.Mesh, Spec.RuntimeModel) : nullptr;
	// The rear pair's own model when the car has one; one that will not load
	// falls back to the front's, which scaled to the rear axle is still a wheel.
	UStaticMesh* Rear = Loaded && Spec.HasRearModel() ? ApexCarContent::LoadMesh(Spec.RearMesh, Spec.RearRuntimeModel) : nullptr;
	if (!Rear)
	{
		Rear = Loaded;
	}
	bHasWheels = Loaded != nullptr;
	MeshBounds = Loaded ? Loaded->GetBounds() : FBoxSphereBounds(ForceInit);
	RearMeshBounds = Rear ? Rear->GetBounds() : FBoxSphereBounds(ForceInit);
	SpinRad[0] = SpinRad[1] = 0.0f;
	SteerRad = 0.0f;
	for (int32 i = 0; i < Components.Num(); ++i)
	{
		UStaticMeshComponent* Wheel = Components[i];
		UStaticMesh* Mesh = ApexWheels::IsFront(static_cast<ApexWheels::EWheel>(i)) ? Loaded : Rear;
		if (Wheel && Wheel->GetStaticMesh() != Mesh)
		{
			// Overrides are per slot index (a skin's rims): not the old model's on the new one.
			Wheel->EmptyOverrideMaterials();
			Wheel->SetStaticMesh(Mesh);
		}
	}
	// A new mesh draws its own tyre; the look is put back on it below.
	const EApexCompoundKind Look = TyreLook;
	TyreLook = EApexCompoundKind::Slick;
	SetVisible(bVisible);
	Place();
	SetTyreLook(Look);
}

void FApexCarWheelSet::SetTyreLook(EApexCompoundKind Kind)
{
	if (Kind == TyreLook)
	{
		return;
	}
	TyreLook = Kind;
	if (!bHasWheels)
	{
		return;
	}
	FLinearColor Colour;
	float Roughness = 0.0f;
	const bool bTreaded = ApexWheels::TreadedTint(Kind, Colour, Roughness);
	FLinearColor Band;
	const bool bBand = ApexWheels::CompoundBandColour(Kind, Band);
	for (int32 i = 0; i < Components.Num(); ++i)
	{
		UStaticMeshComponent* Wheel = Components[i];
		// The model's own sidewall ring, when it has one: green or blue for
		// a rain tyre (this wheel's own instance, as below), the model's
		// colour for a slick.
		const int32 BandIndex = Wheel && Wheel->GetStaticMesh() ? Wheel->GetMaterialIndex(ApexWheels::BandSlot) : INDEX_NONE;
		if (BandIndex != INDEX_NONE)
		{
			if (!bBand)
			{
				Wheel->SetMaterial(BandIndex, nullptr);
			}
			else if (UMaterialInstanceDynamic* Ring = ApexCarContent::OwnMaterialInstance(*Wheel, BandIndex, true))
			{
				Ring->SetVectorParameterValue(ApexCarMaterials::BaseColorFactor, Band);
			}
		}
		if (UsesTyreParent(i))
		{
			// The tyre parent draws the tread itself, from the custom data.
			WriteTyreData(i);
			continue;
		}
		const int32 Index = Wheel && Wheel->GetStaticMesh() ? Wheel->GetMaterialIndex(ApexWheels::TyreSlot) : INDEX_NONE;
		if (Index == INDEX_NONE)
		{
			continue;
		}
		if (!bTreaded)
		{
			// Back to the mesh's own (shared) tyre: the override off.
			Wheel->SetMaterial(Index, nullptr);
			continue;
		}
		// This wheel's own instance: the class wheel's slots are shared by
		// every car on that wheel, and a dynamic instance made from the
		// shared one would repaint them all (ApexCarContent::OwnMaterialInstance).
		if (UMaterialInstanceDynamic* Tyre = ApexCarContent::OwnMaterialInstance(*Wheel, Index, true))
		{
			Tyre->SetVectorParameterValue(ApexCarMaterials::BaseColorFactor, Colour);
			Tyre->SetScalarParameterValue(ApexCarMaterials::RoughnessFactor, Roughness);
		}
	}
}

bool FApexCarWheelSet::UsesTyreParent(int32 Index) const
{
	const UStaticMeshComponent* Wheel = bHasWheels && Components.IsValidIndex(Index) ? Components[Index].Get() : nullptr;
	const int32 Slot = Wheel && Wheel->GetStaticMesh() ? Wheel->GetMaterialIndex(ApexWheels::TyreSlot) : INDEX_NONE;
	if (Slot == INDEX_NONE)
	{
		return false;
	}
	UMaterialInterface* Material = Wheel->GetMaterial(Slot);
	const UMaterial* Base = Material ? Material->GetBaseMaterial() : nullptr;
	return Base && Base->GetFName() == FName(ApexCarMaterials::TyreName);
}

void FApexCarWheelSet::WriteTyreData(int32 Index)
{
	UStaticMeshComponent* Wheel = Components.IsValidIndex(Index) ? Components[Index].Get() : nullptr;
	if (!Wheel || !UsesTyreParent(Index))
	{
		return;
	}
	namespace Cpd = ApexCarMaterials::TyreCpd;
	const ApexWheels::EWheel Which = static_cast<ApexWheels::EWheel>(Index);
	const bool bFront = ApexWheels::IsFront(Which);
	const FVector Extent = (bFront ? MeshBounds : RearMeshBounds).BoxExtent;
	// World cm per body-frame cm: the turntable may scale the car.
	const USceneComponent* Body = Wheel->GetAttachParent();
	const float BodyScale = Body ? static_cast<float>(Body->GetComponentScale().Z) : 1.0f;
	const float Width = bFront ? Spec.FrontWidthM : Spec.RearWidthM;
	const FApexTyreSurface& Surface = Surfaces[Index];
	FLinearColor Band;
	ApexWheels::CompoundBandColour(TyreLook, Band);
	const float Values[Cpd::Count - Cpd::RadiusLocal] = {
		static_cast<float>(FMath::Max(Extent.Y, Extent.Z)),
		static_cast<float>(Extent.X),
		ApexWheels::RadiusM(Spec, Which) * 100.0f * BodyScale,
		Width * 50.0f * BodyScale,
		ApexWheels::TreadIndex(TyreLook),
		// Rolling forward turns the left wheel about its own -X and the
		// turned-round right one about its +X (WheelTransform).
		ApexWheels::IsLeft(Which) ? -1.0f : 1.0f,
		Surface.Wear,
		Surface.Wetness,
		Surface.FlatSpot,
		FlatSpotTheta[Index],
		SagCm[Index] * BodyScale,
		Band.R,
		Band.G,
		Band.B,
		0.0f,
	};
	// Only what changed: each write re-sends the component's render state.
	for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(Values)); ++i)
	{
		const int32 Slot = Cpd::RadiusLocal + i;
		const TArray<float>& Data = Wheel->GetCustomPrimitiveData().Data;
		if (!Data.IsValidIndex(Slot) || FMath::Abs(Data[Slot] - Values[i]) > 1e-3f)
		{
			Wheel->SetCustomPrimitiveDataFloat(Slot, Values[i]);
		}
	}
}

void FApexCarWheelSet::SetTyreSurface(ApexWheels::EWheel Wheel, const FApexTyreSurface& Surface)
{
	const int32 Index = static_cast<int32>(Wheel);
	if (Index < 0 || Index >= ApexWheels::NumWheels)
	{
		return;
	}
	FApexTyreSurface Next = Surface;
	Next.Wear = FMath::Clamp(Next.Wear, 0.0f, 1.0f);
	Next.Wetness = FMath::Clamp(Next.Wetness, 0.0f, 1.0f);
	Next.FlatSpot = FMath::Clamp(Next.FlatSpot, 0.0f, 1.0f);
	// A new flat spot is ground where the locked wheel touches the road
	// now: it does not turn while it is locked. One per tyre; a fresh set
	// (or one worn round again) lays the next wherever it locks.
	constexpr float NewSpot = 0.02f;
	if (Next.FlatSpot >= NewSpot && Surfaces[Index].FlatSpot < NewSpot && Components.IsValidIndex(Index) && Components[Index])
	{
		FlatSpotTheta[Index] = ApexWheels::ContactTheta(Components[Index]->GetComponentTransform());
	}
	Surfaces[Index] = Next;
	const float Sag = ApexWheels::DeflatedShare(Next.PressureKpa) * ApexWheels::MaxSagShare * ApexWheels::RadiusM(Spec, Wheel) * 100.0f;
	if (FMath::Abs(Sag - SagCm[Index]) > 0.05f)
	{
		SagCm[Index] = Sag;
		Place();
	}
	WriteTyreData(Index);
}

bool FApexCarWheelSet::ContactPatch(ApexWheels::EWheel Wheel, FVector& OutWorld) const
{
	const int32 Index = static_cast<int32>(Wheel);
	const UStaticMeshComponent* Component = bHasWheels && Components.IsValidIndex(Index) ? Components[Index].Get() : nullptr;
	if (!Component)
	{
		return false;
	}
	// A deflated tyre's hub is dropped by its squat; the tread is still on the road.
	OutWorld = Component->GetComponentLocation() - FVector::UpVector * (ApexWheels::RadiusM(Spec, Wheel) * 100.0f - SagCm[Index]);
	return true;
}

void FApexCarWheelSet::Update(float SteeringInput, float Distance, float MaxStepRad)
{
	if (!bHasWheels)
	{
		return;
	}
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
			const bool bFront = ApexWheels::IsFront(Wheel);
			FTransform Transform = ApexWheels::WheelTransform(
				Spec, Wheel, bFront ? MeshBounds : RearMeshBounds, SteerRad, SpinRad[bFront ? 0 : 1]);
			// A deflated tyre's hub sits lower; the tyre parent flattens the
			// tread by as much, so it still stands on the road.
			Transform.AddToTranslation(FVector(0.0, 0.0, -SagCm[i]));
			Component->SetRelativeTransform(Transform);
			WriteTyreData(i);
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
