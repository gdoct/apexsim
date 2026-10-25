#include "Race/ApexPhotoMode.h"

#include "Misc/Paths.h"

// Named, not anonymous: this module is unity-built.
namespace ApexPhotoSteps
{
	const float FocalLengths[] = { 12.0f, 16.0f, 20.0f, 24.0f, 28.0f, 35.0f, 50.0f, 70.0f, 85.0f, 105.0f, 135.0f,
		200.0f, 300.0f, 400.0f, 600.0f };
	const float Focus[] = { 0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f, 10.0f, 15.0f, 20.0f, 30.0f, 50.0f, 75.0f,
		100.0f, 150.0f, 200.0f, 300.0f };
	const float Apertures[] = { 0.0f, 1.4f, 2.0f, 2.8f, 4.0f, 5.6f, 8.0f, 11.0f, 16.0f, 22.0f };
	const int32 Resolutions[] = { 1, 2, 4 };

	/**
	 * The step `Direction` from `Value` in an ascending table: from a value on
	 * a step, the next; from one between steps, the neighbour that way. Held
	 * at either end.
	 */
	float Step(TConstArrayView<float> Table, float Value, int32 Direction)
	{
		if (Table.Num() == 0)
		{
			return Value;
		}
		constexpr float Tolerance = 1.0e-3f;
		if (Direction > 0)
		{
			for (const float Entry : Table)
			{
				if (Entry > Value + Tolerance)
				{
					return Entry;
				}
			}
			return Table.Last();
		}
		if (Direction < 0)
		{
			for (int32 Index = Table.Num() - 1; Index >= 0; --Index)
			{
				if (Table[Index] < Value - Tolerance)
				{
					return Table[Index];
				}
			}
			return Table[0];
		}
		return Value;
	}
}

namespace ApexPhoto
{
	float FovForFocalLength(float FocalLengthMm)
	{
		const float Mm = FMath::Max(FocalLengthMm, 1.0f);
		return FMath::RadiansToDegrees(2.0f * FMath::Atan(SensorWidthMm / (2.0f * Mm)));
	}

	float FocalLengthForFov(float FovDeg)
	{
		const float HalfRad = FMath::DegreesToRadians(FMath::Clamp(FovDeg, 1.0f, 170.0f) * 0.5f);
		return FMath::Clamp(SensorWidthMm / (2.0f * FMath::Tan(HalfRad)), MinFocalLengthMm, MaxFocalLengthMm);
	}

	float StepFocalLength(float Mm, int32 Direction)
	{
		return ApexPhotoSteps::Step(ApexPhotoSteps::FocalLengths, Mm, Direction);
	}

	float StepFocus(float FocusM, int32 Direction)
	{
		return ApexPhotoSteps::Step(ApexPhotoSteps::Focus, FocusM, Direction);
	}

	float StepAperture(float Fstop, int32 Direction)
	{
		return ApexPhotoSteps::Step(ApexPhotoSteps::Apertures, Fstop, Direction);
	}

	float StepExposure(float Ev, int32 Direction)
	{
		// In thirds, snapped, so a third up and a third down come back exactly.
		const int32 Thirds = FMath::RoundToInt(Ev * 3.0f) + FMath::Sign(Direction);
		return FMath::Clamp(static_cast<float>(Thirds) / 3.0f, MinExposureEv, MaxExposureEv);
	}

	int32 StepResolution(int32 Multiplier, int32 Direction)
	{
		int32 Index = 0;
		for (int32 Candidate = 0; Candidate < UE_ARRAY_COUNT(ApexPhotoSteps::Resolutions); ++Candidate)
		{
			if (ApexPhotoSteps::Resolutions[Candidate] <= Multiplier)
			{
				Index = Candidate;
			}
		}
		Index = FMath::Clamp(Index + FMath::Sign(Direction), 0, UE_ARRAY_COUNT(ApexPhotoSteps::Resolutions) - 1);
		return ApexPhotoSteps::Resolutions[Index];
	}

	void StepSetting(FLens& Lens, ESetting Setting, int32 Direction)
	{
		switch (Setting)
		{
		case ESetting::FocalLength: Lens.FocalLengthMm = StepFocalLength(Lens.FocalLengthMm, Direction); break;
		case ESetting::Focus:       Lens.FocusM = StepFocus(Lens.FocusM, Direction); break;
		case ESetting::Aperture:    Lens.Fstop = StepAperture(Lens.Fstop, Direction); break;
		case ESetting::Exposure:    Lens.ExposureEv = StepExposure(Lens.ExposureEv, Direction); break;
		case ESetting::Resolution:  Lens.Resolution = StepResolution(Lens.Resolution, Direction); break;
		default: break;
		}
	}

	const TCHAR* SettingName(ESetting Setting)
	{
		switch (Setting)
		{
		case ESetting::FocalLength: return TEXT("FOCAL LENGTH");
		case ESetting::Focus:       return TEXT("FOCUS");
		case ESetting::Aperture:    return TEXT("APERTURE");
		case ESetting::Exposure:    return TEXT("EXPOSURE");
		case ESetting::Resolution:  return TEXT("SHOT SIZE");
		default:                    return TEXT("");
		}
	}

	FString DescribeSetting(const FLens& Lens, ESetting Setting)
	{
		switch (Setting)
		{
		case ESetting::FocalLength:
			return FString::Printf(TEXT("%d mm"), FMath::RoundToInt(Lens.FocalLengthMm));
		case ESetting::Focus:
			return Lens.FocusM <= 0.0f ? FString(TEXT("ON THE CAR"))
				: Lens.FocusM < 10.0f ? FString::Printf(TEXT("%.1f m"), Lens.FocusM)
				: FString::Printf(TEXT("%d m"), FMath::RoundToInt(Lens.FocusM));
		case ESetting::Aperture:
			return Lens.Fstop <= 0.0f ? FString(TEXT("NO BLUR"))
				: FMath::IsNearlyEqual(Lens.Fstop, FMath::RoundToFloat(Lens.Fstop)) ? FString::Printf(TEXT("f/%d"), FMath::RoundToInt(Lens.Fstop))
				: FString::Printf(TEXT("f/%.1f"), Lens.Fstop);
		case ESetting::Exposure:
		{
			const int32 Thirds = FMath::RoundToInt(Lens.ExposureEv * 3.0f);
			if (Thirds == 0)
			{
				return TEXT("0 EV");
			}
			return FString::Printf(TEXT("%s%.1f EV"), Thirds > 0 ? TEXT("+") : TEXT("-"), FMath::Abs(Thirds) / 3.0f);
		}
		case ESetting::Resolution:
			return FString::Printf(TEXT("%dx SCREEN"), Lens.Resolution);
		default:
			return FString();
		}
	}

	ESetting StepRow(ESetting Setting, int32 Direction)
	{
		const int32 Count = static_cast<int32>(ESetting::Count);
		const int32 Index = ((static_cast<int32>(Setting) + FMath::Sign(Direction)) % Count + Count) % Count;
		return static_cast<ESetting>(Index);
	}

	// --- The camera ----------------------------------------------------------------

	FRotator Rotation(const FCamera& Camera)
	{
		return FRotator(Camera.PitchDeg, Camera.YawDeg, 0.0f);
	}

	FCamera FreeAt(const FVector& LocationCm, const FRotator& Rotation)
	{
		FCamera Camera;
		Camera.LocationCm = LocationCm;
		Camera.YawDeg = FRotator::NormalizeAxis(Rotation.Yaw);
		Camera.PitchDeg = FMath::Clamp(FRotator::NormalizeAxis(Rotation.Pitch), -MaxPitchDeg, MaxPitchDeg);
		return Camera;
	}

	namespace Detail
	{
		/** Place an orbiting camera from its angles and distance, looking at the pivot. */
		void PlaceOrbit(FCamera& Camera, const FVector& PivotCm)
		{
			const FVector Out = FRotator(Camera.OrbitPitchDeg, Camera.OrbitYawDeg, 0.0f).Vector();
			Camera.LocationCm = PivotCm + Out * Camera.OrbitDistanceCm;
			const FRotator Look = (-Out).Rotation();
			Camera.YawDeg = FRotator::NormalizeAxis(Look.Yaw);
			Camera.PitchDeg = FMath::Clamp(FRotator::NormalizeAxis(Look.Pitch), -MaxPitchDeg, MaxPitchDeg);
		}
	}

	void SetOrbit(FCamera& Camera, bool bOrbit, const FVector& PivotCm)
	{
		if (Camera.bOrbit == bOrbit)
		{
			return;
		}
		Camera.bOrbit = bOrbit;
		if (!bOrbit)
		{
			// The free camera carries on from where the orbit left it, looking the same way.
			return;
		}
		const FVector Out = Camera.LocationCm - PivotCm;
		Camera.OrbitDistanceCm = FMath::Clamp(static_cast<float>(Out.Size()), MinOrbitCm, MaxOrbitStartCm);
		const FRotator Angles = Out.IsNearlyZero() ? FRotator(10.0f, Camera.YawDeg + 180.0f, 0.0f) : Out.Rotation();
		Camera.OrbitYawDeg = FRotator::NormalizeAxis(Angles.Yaw);
		Camera.OrbitPitchDeg = FMath::Clamp(FRotator::NormalizeAxis(Angles.Pitch), -MaxPitchDeg, MaxPitchDeg);
		Detail::PlaceOrbit(Camera, PivotCm);
	}

	void Advance(FCamera& Camera, const FInput& Input, float DeltaSeconds, const FVector& PivotCm)
	{
		const float Dt = FMath::Max(DeltaSeconds, 0.0f);
		const float Speed = MoveSpeedCmPerS * (Input.bFast ? FastFactor : 1.0f) * (Input.bSlow ? SlowFactor : 1.0f);
		const FVector Move = Input.Move.BoundToCube(1.0f);
		const float TurnYaw = FMath::Clamp(static_cast<float>(Input.Look.X), -1.0f, 1.0f) * LookRateDegPerS * Dt
			+ static_cast<float>(Input.LookDeltaDeg.X);
		const float TurnPitch = FMath::Clamp(static_cast<float>(Input.Look.Y), -1.0f, 1.0f) * LookRateDegPerS * Dt
			+ static_cast<float>(Input.LookDeltaDeg.Y);

		if (Camera.bOrbit)
		{
			// The stick turns the camera round the car, the way a hand turns
			// it: right swings it to the right, so the view turns left.
			Camera.OrbitYawDeg = FRotator::NormalizeAxis(Camera.OrbitYawDeg - TurnYaw);
			Camera.OrbitPitchDeg = FMath::Clamp(Camera.OrbitPitchDeg - TurnPitch + Move.Z * 45.0f * Dt, -MaxPitchDeg, MaxPitchDeg);
			// Forward closes in, in proportion, so a far orbit moves as fast as a near one feels.
			const float Zoom = FMath::Exp(-Move.X * Dt * (Speed / MoveSpeedCmPerS));
			Camera.OrbitDistanceCm = FMath::Clamp(Camera.OrbitDistanceCm * Zoom, MinOrbitCm, MaxRangeCm);
			// Sideways swings round the car too.
			Camera.OrbitYawDeg = FRotator::NormalizeAxis(Camera.OrbitYawDeg - Move.Y * 45.0f * Dt);
			Detail::PlaceOrbit(Camera, PivotCm);
			return;
		}

		Camera.YawDeg = FRotator::NormalizeAxis(Camera.YawDeg + TurnYaw);
		Camera.PitchDeg = FMath::Clamp(Camera.PitchDeg + TurnPitch, -MaxPitchDeg, MaxPitchDeg);
		const FRotator View = Rotation(Camera);
		const FVector Forward = View.Vector();
		const FVector Right = FRotator(0.0f, Camera.YawDeg, 0.0f).RotateVector(FVector::RightVector);
		Camera.LocationCm += (Forward * Move.X + Right * Move.Y + FVector::UpVector * Move.Z) * Speed * Dt;

		const FVector FromPivot = Camera.LocationCm - PivotCm;
		if (FromPivot.SizeSquared() > FMath::Square(MaxRangeCm))
		{
			Camera.LocationCm = PivotCm + FromPivot.GetSafeNormal() * MaxRangeCm;
		}
	}

	bool KeepAboveGround(FCamera& Camera, double GroundZ)
	{
		const double Floor = GroundZ + GroundClearanceCm;
		if (Camera.LocationCm.Z >= Floor)
		{
			return false;
		}
		Camera.LocationCm.Z = Floor;
		return true;
	}

	bool SubjectFromName(const FString& Name, int32& OutWheel)
	{
		static const TCHAR* Wheels[] = { TEXT("fl"), TEXT("fr"), TEXT("rl"), TEXT("rr") };
		const FString Key = Name.TrimStartAndEnd().ToLower();
		if (Key == TEXT("body") || Key == TEXT("car"))
		{
			OutWheel = INDEX_NONE;
			return true;
		}
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Wheels); ++Index)
		{
			if (Key == Wheels[Index])
			{
				OutWheel = Index;
				return true;
			}
		}
		return false;
	}

	// --- The keys --------------------------------------------------------------------

	ECommand CommandFor(const FKey& Key)
	{
		if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom)
		{
			return ECommand::TakeShot;
		}
		if (Key == EKeys::Escape || Key == EKeys::BackSpace || Key == EKeys::P || Key == EKeys::Gamepad_FaceButton_Right
			|| Key == EKeys::Gamepad_Special_Right)
		{
			return ECommand::Exit;
		}
		if (Key == EKeys::O || Key == EKeys::Gamepad_FaceButton_Top)
		{
			return ECommand::Orbit;
		}
		if (Key == EKeys::H || Key == EKeys::Gamepad_Special_Left)
		{
			return ECommand::HidePanel;
		}
		if (Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up)
		{
			return ECommand::PreviousRow;
		}
		if (Key == EKeys::Down || Key == EKeys::Gamepad_DPad_Down)
		{
			return ECommand::NextRow;
		}
		if (Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left)
		{
			return ECommand::Decrease;
		}
		if (Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Right)
		{
			return ECommand::Increase;
		}
		if (Key == EKeys::SpaceBar || Key == EKeys::Gamepad_FaceButton_Left)
		{
			return ECommand::PlayPause;
		}
		if (Key == EKeys::R || Key == EKeys::Gamepad_RightThumbstick)
		{
			return ECommand::Reset;
		}
		if (Key == EKeys::Hyphen || Key == EKeys::Subtract || Key == EKeys::Gamepad_LeftShoulder)
		{
			return ECommand::ZoomOut;
		}
		if (Key == EKeys::Equals || Key == EKeys::Add || Key == EKeys::Gamepad_RightShoulder)
		{
			return ECommand::ZoomIn;
		}
		return ECommand::None;
	}

	bool HeldAxisFor(const FKey& Key, FVector& OutMove, FVector2D& OutLook)
	{
		OutMove = FVector::ZeroVector;
		OutLook = FVector2D::ZeroVector;
		if (Key == EKeys::W)      { OutMove.X = 1.0; }
		else if (Key == EKeys::S) { OutMove.X = -1.0; }
		else if (Key == EKeys::D) { OutMove.Y = 1.0; }
		else if (Key == EKeys::A) { OutMove.Y = -1.0; }
		else if (Key == EKeys::E) { OutMove.Z = 1.0; }
		else if (Key == EKeys::Q) { OutMove.Z = -1.0; }
		else if (Key == EKeys::L) { OutLook.X = 1.0; }
		else if (Key == EKeys::J) { OutLook.X = -1.0; }
		else if (Key == EKeys::I) { OutLook.Y = 1.0; }
		else if (Key == EKeys::K) { OutLook.Y = -1.0; }
		else
		{
			return false;
		}
		return true;
	}

	// --- The shot --------------------------------------------------------------------

	int32 FitMultiplier(FIntPoint ViewportSize, int32 Wanted, int32 MaxDimension)
	{
		int32 Multiplier = FMath::Max(Wanted, 1);
		const int32 Largest = FMath::Max(FMath::Max(ViewportSize.X, ViewportSize.Y), 1);
		while (Multiplier > 1 && Largest * Multiplier > MaxDimension)
		{
			Multiplier /= 2;
		}
		return FMath::Max(Multiplier, 1);
	}

	FString MakeShotName(const FDateTime& When, const FString& TrackStem, int32 Multiplier)
	{
		FString Stem;
		for (const TCHAR Char : TrackStem)
		{
			if (FChar::IsAlnum(Char) || Char == TEXT('-'))
			{
				Stem.AppendChar(Char);
			}
		}
		return FString::Printf(TEXT("ApexSim_%s%s%s_%dx"), *Stem, Stem.IsEmpty() ? TEXT("") : TEXT("_"),
			*When.ToString(TEXT("%Y%m%d_%H%M%S")), FMath::Max(Multiplier, 1));
	}

	FString ShotFolder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("ApexSim"));
	}
}
