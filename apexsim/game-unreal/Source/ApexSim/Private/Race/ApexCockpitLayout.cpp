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
			Layout.LeftMirrorSizeCm = Layout.RightMirrorSizeCm = FVector2D(14.0f, 7.0f);
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
			Layout.LeftMirrorSizeCm = Layout.RightMirrorSizeCm = FVector2D(16.0f, 10.0f);
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
		auto OverrideSize = [](FVector2D& Target, const FVector2D& Value)
		{
			if (Value.X > 0.0f && Value.Y > 0.0f)
			{
				Target = Value;
			}
		};
		OverrideSize(Layout.CentreMirrorSizeCm, Overrides.MirrorCentreSizeCm);
		OverrideSize(Layout.LeftMirrorSizeCm, Overrides.MirrorLeftSizeCm);
		OverrideSize(Layout.RightMirrorSizeCm, Overrides.MirrorRightSizeCm);
		if (Overrides.WheelRakeDeg != 0.0f)
		{
			Layout.WheelRakeDeg = Overrides.WheelRakeDeg;
		}
		if (Overrides.WheelLockDeg > 0.0f)
		{
			Layout.WheelLockDeg = Overrides.WheelLockDeg;
		}
		Layout.bRigWheel = Overrides.bRigWheel;
		Layout.bRigDash = Overrides.bRigDash;
		Layout.SteeringWheelModel = Overrides.RuntimeSteeringWheel;
		if (!Layout.SteeringWheelModel.IsEmpty() && Overrides.RuntimeDashWidthCm > 0.0f)
		{
			Layout.DashCm = Overrides.RuntimeDashCm;
			Layout.DashWidthCm = Overrides.RuntimeDashWidthCm;
		}
		if (!Layout.SteeringWheelModel.IsEmpty())
		{
			Layout.HandsModel = Overrides.RuntimeHands;
			Layout.WristCm = Overrides.RuntimeWristCm;
			Layout.ForearmModel = Overrides.RuntimeForearm;
			Layout.UpperArmModel = Overrides.RuntimeUpperArm;
		}

		return Layout;
	}

	FVector ShoulderFromEye(const FVector& Eye, float Side)
	{
		// A seated driver's shoulder sits a hand behind the eye, a head and a
		// neck below it and a shoulder's width out.
		return Eye + FVector(-9.0f, 19.0f * Side, -26.0f);
	}

	FVector SolveElbow(const FVector& Shoulder, const FVector& Wrist, float UpperCm, float ForeCm, const FVector& Pole)
	{
		const FVector Reach = Wrist - Shoulder;
		const float Distance = Reach.Size();
		if (Distance < UE_KINDA_SMALL_NUMBER)
		{
			return Shoulder + Pole.GetSafeNormal() * UpperCm;
		}
		const FVector Along = Reach / Distance;
		const float Max = UpperCm + ForeCm;
		const float Min = FMath::Abs(UpperCm - ForeCm) + 0.5f;
		const float D = FMath::Clamp(Distance, Min, Max);
		// The elbow's distance along the reach and out from it (two circles).
		const float A = (UpperCm * UpperCm - ForeCm * ForeCm + D * D) / (2.0f * D);
		const float H = FMath::Sqrt(FMath::Max(UpperCm * UpperCm - A * A, 0.0f));
		FVector Out = Pole - Along * FVector::DotProduct(Pole, Along);
		if (!Out.Normalize())
		{
			Out = FVector::CrossProduct(Along, FVector::RightVector).GetSafeNormal();
		}
		return Shoulder + Along * A + Out * H;
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

	float SeatStiffness(bool bOpenWheel)
	{
		// A closed car's seat, mounts and padding take the edge off; a tub
		// carries the road to the helmet.
		return bOpenWheel ? 1.0f : 0.65f;
	}

	FHeadShakeOffset StepHeadShake(FHeadShake& State, const FHeadShakeInput& In, float DeltaSeconds, float Amount)
	{
		const float Scale = FMath::Clamp(Amount, 0.0f, 1.0f);
		if (Scale <= 0.0f || !(DeltaSeconds > 0.0f))
		{
			if (Scale <= 0.0f)
			{
				State = FHeadShake();
			}
			return FHeadShakeOffset();
		}

		// The neck: a soft, well-damped spring that holds the gaze.
		constexpr float NeckOmega = UE_TWO_PI * 2.5f;
		constexpr float NeckDamping = 0.5f;
		// The helmet against the seat back: stiff and lightly damped, the
		// road's texture.
		constexpr float TextureOmega = UE_TWO_PI * 9.0f;
		constexpr float TextureDamping = 0.3f;
		constexpr float TextureShare = 0.35f;
		// The nod.
		constexpr float PitchOmega = UE_TWO_PI * 3.0f;
		constexpr float PitchDamping = 0.5f;
		// What a suspension's speed does to the chassis, per second: a
		// corner's damper on a quarter of the car (about 8 kN s/m on 300 kg).
		constexpr float ChassisPerS = 28.0f;
		// The nod per m/s of the front against the rear, deg/s².
		constexpr float PitchPerMps = 240.0f;
		// Porpoising (`aero.rs`): its frequency, full swing and the ride
		// height it moves the car through there.
		constexpr float PorpoiseHz = 5.0f;
		constexpr float PorpoiseMaxSwing = 0.25f;
		constexpr float PorpoiseHeightM = 0.015f;
		// The engine: half a millimetre at idle, less as it revs, at twice
		// the crank's rate.
		constexpr float BuzzIdleM = 0.0005f;
		constexpr float BuzzOrders = 2.0f;
		constexpr float StepS = 1.0f / 500.0f;
		// The server's suspension speed is the compression's own rate with
		// no wheel mass, so a kerb's edge or a landing reads as the 6 m/s cap
		// for a tick; a damper's shaft rarely passes a metre a second, so
		// each wheel's is eased into 0.6 m/s.
		constexpr float SuspensionLimitMps = 0.6f;

		float V[4];
		for (int32 Wheel = 0; Wheel < 4; ++Wheel)
		{
			V[Wheel] = SuspensionLimitMps * FMath::Tanh(In.SuspensionVelocityMps[Wheel] / SuspensionLimitMps);
		}
		const float Stiffness = FMath::Clamp(In.Stiffness, 0.0f, 1.0f);
		const float Heave = (V[0] + V[1] + V[2] + V[3]) * 0.25f;
		const float FrontRear = (V[0] + V[1] - V[2] - V[3]) * 0.5f;
		const float RoadAccel = Heave * ChassisPerS * Stiffness;
		const float PorpoiseSwing = FMath::Clamp(In.PorpoiseAmp / PorpoiseMaxSwing, 0.0f, 1.0f) * PorpoiseHeightM;
		const float PorpoiseOmega = UE_TWO_PI * PorpoiseHz;

		const int32 Steps = FMath::Clamp(FMath::CeilToInt32(DeltaSeconds / StepS), 1, 200);
		const float H = DeltaSeconds / static_cast<float>(Steps);
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			State.PorpoisePhase = FMath::Fmod(State.PorpoisePhase + PorpoiseOmega * H, UE_TWO_PI);
			// The body's own heave, up positive: the head lags it.
			const float BodyAccel = In.VerticalAccelMps2 + RoadAccel
				- PorpoiseSwing * PorpoiseOmega * PorpoiseOmega * FMath::Sin(State.PorpoisePhase);

			// Semi-implicit Euler: stable for these springs at this step.
			State.NeckMps += (-BodyAccel - NeckOmega * NeckOmega * State.NeckM - 2.0f * NeckDamping * NeckOmega * State.NeckMps) * H;
			State.NeckM += State.NeckMps * H;
			State.TextureMps += (-RoadAccel * TextureShare - TextureOmega * TextureOmega * State.TextureM
				- 2.0f * TextureDamping * TextureOmega * State.TextureMps) * H;
			State.TextureM += State.TextureMps * H;
			State.PitchDegps += (FrontRear * PitchPerMps * Stiffness - PitchOmega * PitchOmega * State.PitchDeg
				- 2.0f * PitchDamping * PitchOmega * State.PitchDegps) * H;
			State.PitchDeg += State.PitchDegps * H;
		}
		State.NeckM = FMath::Clamp(State.NeckM, -0.05f, 0.05f);
		State.TextureM = FMath::Clamp(State.TextureM, -0.008f, 0.008f);
		State.PitchDeg = FMath::Clamp(State.PitchDeg, -0.6f, 0.6f);

		float Buzz = 0.0f;
		if (In.EngineRpm > 300.0f)
		{
			State.BuzzPhase = FMath::Fmod(State.BuzzPhase + UE_TWO_PI * In.EngineRpm / 60.0f * BuzzOrders * DeltaSeconds, UE_TWO_PI);
			const float Idle = FMath::Lerp(1.0f, 0.4f, FMath::Clamp(In.EngineRpm / 9000.0f, 0.0f, 1.0f));
			Buzz = BuzzIdleM * Idle * Stiffness * FMath::Sin(State.BuzzPhase);
		}

		FHeadShakeOffset Out;
		Out.OffsetCm = FVector(0.0f, 0.0f, (State.NeckM + State.TextureM + Buzz) * 100.0f * Scale);
		Out.PitchDeg = State.PitchDeg * Scale;
		return Out;
	}

	float ExposureToEv100(float Exposure)
	{
		return FMath::Log2(1.0f / (1.2f * FMath::Max(Exposure, UE_SMALL_NUMBER)));
	}

	float ScreenNitsAt(float BaseNits, float Exposure, float ReferenceEv)
	{
		const float Base = FMath::Max(BaseNits, 1.0f);
		if (!(Exposure > 0.0f))
		{
			return Base;
		}
		const float StopsDown = FMath::Max(0.0f, ReferenceEv - ExposureToEv100(Exposure));
		return FMath::Max(1.0f, Base * FMath::Exp2(-StopsDown));
	}
}
