#include "Race/ApexCockpitLayout.h"

namespace ApexCockpit
{
	FBox ActorFrameBox(const FBoxSphereBounds& MeshLocalBounds, const FTransform& MeshRelativeTransform)
	{
		if (MeshLocalBounds.BoxExtent.IsNearlyZero())
		{
			return FallbackBox();
		}
		return MeshLocalBounds.GetBox().TransformBy(MeshRelativeTransform);
	}

	FBox FallbackBox()
	{
		// A GT3's footprint: 4.5 m long, 1.9 m wide, 1.3 m tall, on the ground.
		return FBox(FVector(-225.0f, -95.0f, 0.0f), FVector(225.0f, 95.0f, 130.0f));
	}

	EApexCockpitStyle ResolveStyle(EApexCockpitStyle Requested, const FString& CarClass, const FBox& Box)
	{
		if (Requested != EApexCockpitStyle::Auto)
		{
			return Requested;
		}

		const FString Class = CarClass.ToUpper();
		if (Class.Contains(TEXT("F1")) || Class.Contains(TEXT("FORMULA")) || Class.Contains(TEXT("OPEN"))
			|| Class.Contains(TEXT("INDY")))
		{
			return EApexCockpitStyle::OpenWheel;
		}
		if (!Class.IsEmpty())
		{
			return EApexCockpitStyle::Closed;
		}

		// No class to go on (AI cars, the fallback mesh): a body under 1.1 m
		// has no roof to sit under.
		const float Height = Box.GetSize().Z;
		return Height > 0.0f && Height < 110.0f ? EApexCockpitStyle::OpenWheel : EApexCockpitStyle::Closed;
	}

	FApexCockpitLayout DeriveLayout(const FBox& Box, EApexCockpitStyle Style, const FApexCockpitOverrides& Overrides)
	{
		const FBox Body = Box.IsValid && !Box.GetSize().IsNearlyZero() ? Box : FallbackBox();
		const FVector Size = Body.GetSize();
		const FVector Centre = Body.GetCenter();
		const float Floor = Body.Min.Z;

		FApexCockpitLayout Layout;
		Layout.bOpenWheel = ResolveStyle(Style, FString(), Body) == EApexCockpitStyle::OpenWheel;
		Layout.RoofZ = Body.Max.Z;

		if (Layout.bOpenWheel)
		{
			// Reclined, low, a good way behind the centre of the car, and
			// straddling the centreline.
			Layout.Eye = FVector(Centre.X - 0.08f * Size.X, Centre.Y, Floor + 0.82f * Size.Z);
			// A racing wheel sits high: about 20° below the eye line, close in.
			Layout.Wheel = Layout.Eye + FVector(38.0f, 0.0f, -14.0f);
			Layout.WheelRakeDeg = 12.0f;
			Layout.WheelLockDeg = 90.0f;
			Layout.WheelHalfWidthCm = 13.0f;

			// Mirrors on the sidepod shoulders: nothing on the centreline to
			// hang one from. Far enough ahead to sit inside a 96° view — a
			// mirror the driver has to turn to see is not a mirror.
			Layout.bCentreMirror = false;
			Layout.MirrorCentre = FVector::ZeroVector;
			const float MirrorY = 0.25f * Size.Y;
			Layout.MirrorLeft = FVector(Layout.Eye.X + 70.0f, Centre.Y - MirrorY, Layout.Eye.Z - 8.0f);
			Layout.MirrorRight = FVector(Layout.Eye.X + 70.0f, Centre.Y + MirrorY, Layout.Eye.Z - 8.0f);
			Layout.SideMirrorSizeCm = FVector2D(14.0f, 7.0f);
		}
		else
		{
			// Left-hand drive, sat just behind the middle of the car, eyes
			// about 70% of the way up the body.
			Layout.Eye = FVector(Centre.X - 0.05f * Size.X, Centre.Y - 0.18f * Size.Y, Floor + 0.70f * Size.Z);
			Layout.Wheel = Layout.Eye + FVector(40.0f, 0.0f, -18.0f);
			Layout.WheelRakeDeg = 22.0f;
			Layout.WheelLockDeg = 135.0f;
			Layout.WheelHalfWidthCm = 16.0f;

			Layout.bCentreMirror = true;
			Layout.MirrorCentre = FVector(Layout.Eye.X + 45.0f, Centre.Y, Layout.Eye.Z + 12.0f);
			Layout.CentreMirrorSizeCm = FVector2D(26.0f, 8.0f);
			// Door mirrors: just outside the body, a little below eye height,
			// a metre ahead — at the foot of the A-pillar, where a 96° view
			// still catches them at its edges.
			const float MirrorY = 0.5f * Size.Y + 9.0f;
			Layout.MirrorLeft = FVector(Layout.Eye.X + 105.0f, Centre.Y - MirrorY, Layout.Eye.Z - 12.0f);
			Layout.MirrorRight = FVector(Layout.Eye.X + 105.0f, Centre.Y + MirrorY, Layout.Eye.Z - 12.0f);
			Layout.SideMirrorSizeCm = FVector2D(16.0f, 10.0f);
		}

		auto Override = [](FVector& Target, const FVector& Value)
		{
			if (!Value.IsNearlyZero())
			{
				Target = Value;
			}
		};
		Override(Layout.Eye, Overrides.Eye);
		Override(Layout.Wheel, Overrides.Wheel);
		Override(Layout.MirrorLeft, Overrides.MirrorLeft);
		Override(Layout.MirrorRight, Overrides.MirrorRight);
		if (!Overrides.MirrorCentre.IsNearlyZero())
		{
			// A hand-placed centre mirror is a centre mirror, whatever the style.
			Layout.MirrorCentre = Overrides.MirrorCentre;
			Layout.bCentreMirror = true;
		}

		return Layout;
	}

	FQuat ViewRotation(const FRotator& CarRotation, float HorizonLock, float ViewPitchDeg, float LookYawDeg)
	{
		const float Ride = 1.0f - FMath::Clamp(HorizonLock, 0.0f, 1.0f);
		const FQuat Base = FRotator(CarRotation.Pitch * Ride, CarRotation.Yaw, CarRotation.Roll * Ride).Quaternion();
		// Gaze and head turn compose after the car so they stay in its frame.
		const FQuat Head = FRotator(ViewPitchDeg, LookYawDeg, 0.0f).Quaternion();
		return Base * Head;
	}

	FVector HeadLean(float LateralG, float LongitudinalG, float Amount)
	{
		const float Scale = FMath::Clamp(Amount, 0.0f, 1.0f);
		// Inertia: the head keeps going where the car was going. Deceleration
		// (negative longitudinal g) moves it forward, +X; acceleration to the
		// right (+Y) moves it left.
		const float Forward = FMath::Clamp(-LongitudinalG * 3.0f, -5.0f, 5.0f);
		const float Sideways = FMath::Clamp(-LateralG * 3.5f, -6.0f, 6.0f);
		return FVector(Forward, Sideways, 0.0f) * Scale;
	}
}
