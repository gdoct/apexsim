#include "Race/ApexPhysicsLib.h"

#include "ApexProtocolTypes.h"

namespace ApexPhysics
{
#if WITH_APEX_PHYSICS
	namespace
	{
		FString PhysicsError(const TCHAR* What)
		{
			const FString Why = LastError();
			return Why.IsEmpty() ? FString(What) : FString::Printf(TEXT("%s: %s"), What, *Why);
		}
	}

	bool IsAvailable(FString* OutWhy)
	{
		const uint32 Abi = apx_abi_version();
		if (Abi != APX_ABI_VERSION)
		{
			if (OutWhy)
			{
				*OutWhy = FString::Printf(TEXT("the physics library speaks ABI %u, this client %u: rebuild it (scripts/build_physics_lib.ps1)"),
					Abi, APX_ABI_VERSION);
			}
			return false;
		}
		return true;
	}

	FString LastError()
	{
		const char* Message = apx_last_error();
		return Message ? FString(UTF8_TO_TCHAR(Message)) : FString();
	}

	FTrack::~FTrack()
	{
		if (Track)
		{
			apx_track_free(Track);
		}
	}

	TUniquePtr<FTrack> FTrack::Load(const FString& YamlPath, uint8 RoadContact, TArrayView<const uint8> Conditions,
		FString& OutError)
	{
		ApxTrack* Handle = nullptr;
		const FTCHARToUTF8 Path(*YamlPath);
		const ApxStatus Status = apx_track_load(Path.Get(), RoadContact,
			Conditions.Num() > 0 ? Conditions.GetData() : nullptr, static_cast<size_t>(Conditions.Num()), &Handle);
		if (Status != APX_OK || !Handle)
		{
			OutError = PhysicsError(TEXT("apx_track_load"));
			return nullptr;
		}
		TUniquePtr<FTrack> Out(new FTrack());
		Out->Track = Handle;
		return Out;
	}

	TUniquePtr<FTrack> FTrack::LoadBundle(TArrayView<const uint8> Bundle, uint32 ExpectedCrc, uint8 RoadContact,
		TArrayView<const uint8> Conditions, FString& OutError)
	{
		ApxTrack* Handle = nullptr;
		const ApxStatus Status = apx_track_load_bundle(Bundle.GetData(), static_cast<size_t>(Bundle.Num()), ExpectedCrc,
			RoadContact, Conditions.Num() > 0 ? Conditions.GetData() : nullptr, static_cast<size_t>(Conditions.Num()), &Handle);
		if (Status != APX_OK || !Handle)
		{
			OutError = PhysicsError(TEXT("apx_track_load_bundle"));
			return nullptr;
		}
		TUniquePtr<FTrack> Out(new FTrack());
		Out->Track = Handle;
		return Out;
	}

	bool TrackFilesCrc(const FString& YamlPath, uint8 RoadContact, uint32& OutCrc, FString& OutError)
	{
		const FTCHARToUTF8 Path(*YamlPath);
		if (apx_track_files_crc(Path.Get(), RoadContact, &OutCrc) != APX_OK)
		{
			OutError = PhysicsError(TEXT("apx_track_files_crc"));
			return false;
		}
		return true;
	}

	bool BuildTrackBundle(const FString& YamlPath, uint8 RoadContact, TArray<uint8>& OutBundle, uint32& OutCrc,
		FString& OutError)
	{
		const FTCHARToUTF8 Path(*YamlPath);
		size_t Size = 0;
		ApxStatus Status = apx_track_bundle_build(Path.Get(), RoadContact, nullptr, 0, &Size, &OutCrc);
		if (Status == APX_ERR_BUFFER)
		{
			OutBundle.SetNumUninitialized(static_cast<int32>(Size));
			Status = apx_track_bundle_build(Path.Get(), RoadContact, OutBundle.GetData(), OutBundle.Num(), &Size, &OutCrc);
		}
		if (Status != APX_OK)
		{
			OutBundle.Reset();
			OutError = PhysicsError(TEXT("apx_track_bundle_build"));
			return false;
		}
		return true;
	}

	bool ShowcaseNames(const FString& ListPath, TArray<FString>& OutNames, FString& OutError)
	{
		const FTCHARToUTF8 Path(*ListPath);
		size_t Size = 0;
		ApxStatus Status = apx_showcase_names(Path.Get(), nullptr, 0, &Size);
		TArray<uint8> Text;
		if (Status == APX_ERR_BUFFER)
		{
			Text.SetNumUninitialized(static_cast<int32>(Size));
			Status = apx_showcase_names(Path.Get(), Text.GetData(), Text.Num(), &Size);
		}
		OutNames.Reset();
		if (Status != APX_OK)
		{
			OutError = PhysicsError(TEXT("apx_showcase_names"));
			return false;
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Text.GetData()), Text.Num());
		const FString Joined(Converted.Length(), Converted.Get());
		Joined.ParseIntoArray(OutNames, TEXT("\n"), true);
		return true;
	}

	EShowcaseRender RenderShowcase(const FString& ListPath, const FString& TracksDir, const FString& CarsDir,
		const FString& Name, const FString& OutPath, const volatile uint8* Cancel, FString& OutError)
	{
		const FTCHARToUTF8 List(*ListPath), Tracks(*TracksDir), Cars(*CarsDir), ShowcaseName(*Name), Out(*OutPath);
		const ApxStatus Status = apx_showcase_render(List.Get(), Tracks.Get(), Cars.Get(), ShowcaseName.Get(), Out.Get(), Cancel);
		if (Status == APX_OK)
		{
			return EShowcaseRender::Written;
		}
		OutError = PhysicsError(TEXT("apx_showcase_render"));
		return Status == APX_ERR_CANCELLED ? EShowcaseRender::Cancelled : EShowcaseRender::Failed;
	}

	bool FTrack::SetRoadRows(uint32 FirstCell, TArrayView<const uint8> Rows, TArrayView<const uint8> SnowRows,
		TArrayView<const uint8> LaneSnow, FString& OutError)
	{
		const ApxStatus Status = apx_track_road_rows(Track, FirstCell, Rows.GetData(), Rows.Num(), SnowRows.GetData(),
			SnowRows.Num(), LaneSnow.GetData(), LaneSnow.Num());
		if (Status != APX_OK)
		{
			OutError = PhysicsError(TEXT("apx_track_road_rows"));
			return false;
		}
		return true;
	}

	FCar::~FCar()
	{
		if (Car)
		{
			apx_car_free(Car);
		}
	}

	TUniquePtr<FCar> FCar::Create(const FTrack& Track, TArrayView<const uint8> CarToml, TArrayView<const int8> Clicks,
		int32 GridSlot, FString& OutError)
	{
		ApxCar* Handle = nullptr;
		const ApxStatus Status = apx_car_create(Track.Handle(), CarToml.GetData(), CarToml.Num(),
			Clicks.Num() > 0 ? Clicks.GetData() : nullptr, Clicks.Num(), static_cast<uint8>(FMath::Clamp(GridSlot, 0, 255)),
			&Handle);
		if (Status != APX_OK || !Handle)
		{
			OutError = PhysicsError(TEXT("apx_car_create"));
			return nullptr;
		}
		TUniquePtr<FCar> Out(new FCar());
		Out->Car = Handle;
		return Out;
	}

	bool FCar::Step(FTrack& Track, const ApxInput& Input, uint32 Tick, int32 TickRateHz)
	{
		return apx_car_step(Car, Track.Handle(), &Input, Tick, static_cast<uint16>(FMath::Clamp(TickRateHz, 1, 65535)))
			== APX_OK;
	}

	bool FCar::Pose(ApxCarPose& Out) const
	{
		return apx_car_pose(Car, &Out) == APX_OK;
	}

	bool FCar::Snapshot(ApxCarSnapshot& Out) const
	{
		return apx_car_snapshot(Car, &Out) == APX_OK;
	}

	bool FCar::ApplySnapshot(const ApxCarSnapshot& Snapshot)
	{
		return apx_car_snapshot_apply(Car, &Snapshot) == APX_OK;
	}

	bool FCar::Save(TArray<uint8>& Out) const
	{
		// The blob's size moves a little from tick to tick: write into what
		// is there and grow only when the library says it is short.
		if (Out.Num() < 4096)
		{
			Out.SetNumUninitialized(8192, EAllowShrinking::No);
		}
		size_t Written = 0;
		ApxStatus Status = apx_car_state_save(Car, Out.GetData(), Out.Num(), &Written);
		if (Status == APX_ERR_BUFFER)
		{
			Out.SetNumUninitialized(static_cast<int32>(Written) + 1024, EAllowShrinking::No);
			Status = apx_car_state_save(Car, Out.GetData(), Out.Num(), &Written);
		}
		if (Status != APX_OK)
		{
			Out.Reset();
			return false;
		}
		Out.SetNum(static_cast<int32>(Written), EAllowShrinking::No);
		return true;
	}

	bool FCar::Load(TArrayView<const uint8> Blob)
	{
		return apx_car_state_load(Car, Blob.GetData(), Blob.Num()) == APX_OK;
	}

	bool FCar::TakeFeedback(ApxFeedback& Out)
	{
		return apx_car_feedback_take(Car, &Out) == APX_OK;
	}
#else
	namespace
	{
		const TCHAR* NotBuilt = TEXT("the client was built without the physics library (run scripts/build_physics_lib.ps1, then build the client)");
	}

	bool IsAvailable(FString* OutWhy)
	{
		if (OutWhy)
		{
			*OutWhy = NotBuilt;
		}
		return false;
	}

	FString LastError() { return NotBuilt; }

	bool ShowcaseNames(const FString&, TArray<FString>& OutNames, FString& OutError)
	{
		OutNames.Reset();
		OutError = NotBuilt;
		return false;
	}

	EShowcaseRender RenderShowcase(const FString&, const FString&, const FString&, const FString&, const FString&,
		const volatile uint8*, FString& OutError)
	{
		OutError = NotBuilt;
		return EShowcaseRender::Failed;
	}

	FTrack::~FTrack() = default;

	TUniquePtr<FTrack> FTrack::Load(const FString&, uint8, TArrayView<const uint8>, FString& OutError)
	{
		OutError = NotBuilt;
		return nullptr;
	}

	bool FTrack::SetRoadRows(uint32, TArrayView<const uint8>, TArrayView<const uint8>, TArrayView<const uint8>, FString& OutError)
	{
		OutError = NotBuilt;
		return false;
	}

	TUniquePtr<FTrack> FTrack::LoadBundle(TArrayView<const uint8>, uint32, uint8, TArrayView<const uint8>, FString& OutError)
	{
		OutError = NotBuilt;
		return nullptr;
	}

	bool TrackFilesCrc(const FString&, uint8, uint32& OutCrc, FString& OutError)
	{
		OutCrc = 0;
		OutError = NotBuilt;
		return false;
	}

	bool BuildTrackBundle(const FString&, uint8, TArray<uint8>& OutBundle, uint32& OutCrc, FString& OutError)
	{
		OutBundle.Reset();
		OutCrc = 0;
		OutError = NotBuilt;
		return false;
	}

	FCar::~FCar() = default;

	TUniquePtr<FCar> FCar::Create(const FTrack&, TArrayView<const uint8>, TArrayView<const int8>, int32, FString& OutError)
	{
		OutError = NotBuilt;
		return nullptr;
	}

	bool FCar::Step(FTrack&, const ApxInput&, uint32, int32) { return false; }
	bool FCar::Pose(ApxCarPose&) const { return false; }
	bool FCar::Snapshot(ApxCarSnapshot&) const { return false; }
	bool FCar::ApplySnapshot(const ApxCarSnapshot&) { return false; }
	bool FCar::Save(TArray<uint8>& Out) const { Out.Reset(); return false; }
	bool FCar::Load(TArrayView<const uint8>) { return false; }
	bool FCar::TakeFeedback(ApxFeedback& Out) { Out = ApxFeedback{}; return false; }
#endif

	FApexDriverFeedback ToFeedback(const ApxFeedback& In, int64 Tick)
	{
		FApexDriverFeedback Out;
		Out.ServerTick = Tick;
		const int32 Samples = FMath::Min<int32>(static_cast<int32>(In.steer_samples), APX_FEEDBACK_SAMPLES);
		Out.SteerTorque.Append(In.steer_torque, Samples);
		for (int32 Wheel = 0; Wheel < 4; ++Wheel)
		{
			FApexWheelFeedback& W = Out.Wheels[Wheel];
			W.SlipRatio = In.slip_ratio[Wheel];
			W.SlipAngle = In.slip_angle[Wheel];
			// As the codec reads the wire: a surface this client does not know is at least not road.
			W.Surface = static_cast<EApexContactSurface>(FMath::Min<uint8>(In.surface[Wheel], static_cast<uint8>(EApexContactSurface::Off)));
			W.SuspensionMps = In.suspension_mps[Wheel];
			W.FlatSpot = FMath::Min<uint8>(In.flat_spot[Wheel], 100) / 100.0f;
		}
		Out.bAbsActive = In.abs_active != 0;
		Out.bTcActive = In.tc_active != 0;
		Out.ImpactMps = In.impact_mps;
		Out.SteerKick = In.steer_kick;
		Out.SteerInput = In.steer_input;
		Out.SteerStiffness = In.steer_stiffness;
		Out.FrontLoad = In.front_load;
		return Out;
	}

	ApxInput ToInput(const FApexPlayerInput& Input)
	{
		ApxInput Out = {};
		Out.throttle = Input.Throttle;
		Out.brake = Input.Brake;
		// The wire's sign, as SendPlayerInput writes it: positive left.
		Out.steering = Input.Steering;
		Out.has_clutch = Input.HasClutch() ? 1 : 0;
		Out.clutch = Input.HasClutch() ? FMath::Clamp(Input.Clutch, 0.0f, 1.0f) : 0.0f;
		Out.has_gear = Input.HasGear() ? 1 : 0;
		Out.gear = Input.HasGear() ? static_cast<int8>(FMath::Clamp(Input.Gear, -1, 127)) : 0;
		Out.drs = Input.bDrs ? 1 : 0;
		Out.headlights = Input.Headlights < 0 ? -1 : (Input.Headlights > 0 ? 1 : 0);
		Out.flash = Input.bFlash ? 1 : 0;
		Out.ers_mode = Input.ErsMode < 0 ? -1 : static_cast<int8>(FMath::Clamp(Input.ErsMode, 0, 2));
		Out.ers_boost = Input.bErsBoost ? 1 : 0;
		Out.handbrake = Input.bHandbrake ? 1 : 0;
		return Out;
	}

	ApxCarSnapshot ToSnapshot(const FApexOwnCarSnapshot& In)
	{
		ApxCarSnapshot Out = {};
		for (int32 i = 0; i < 3; ++i)
		{
			Out.pos[i] = In.Pos[i];
			Out.attitude[i] = In.Attitude[i];
			Out.vel[i] = In.Vel[i];
			Out.angular_vel[i] = In.AngularVel[i];
			Out.g_forces[i] = In.GForces[i];
		}
		for (int32 i = 0; i < 4; ++i)
		{
			Out.wheel_angular_vel[i] = In.WheelAngularVel[i];
			Out.wheel_load_n[i] = In.WheelLoadN[i];
			Out.suspension_travel_m[i] = In.SuspensionTravelM[i];
			Out.suspension_velocity_mps[i] = In.SuspensionVelocityMps[i];
			Out.controls[i] = In.Controls[i];
		}
		Out.speed_mps = In.SpeedMps;
		Out.vertical_accel_mps2 = In.VerticalAccelMps2;
		Out.ground_slope[0] = In.GroundSlope[0];
		Out.ground_slope[1] = In.GroundSlope[1];
		Out.porpoise[0] = In.Porpoise[0];
		Out.porpoise[1] = In.Porpoise[1];
		Out.engine_rpm = In.EngineRpm;
		Out.turbo_spool = In.TurboSpool;
		Out.track_progress = In.TrackProgress;
		Out.lateral_offset_m = In.LateralOffsetM;
		Out.centerline_idx = In.CenterlineIdx < 0 ? APX_NO_INDEX : static_cast<uint32>(In.CenterlineIdx);
		Out.auto_shift_hold_ticks = static_cast<uint16>(FMath::Clamp(In.AutoShiftHoldTicks, 0, 65535));
		Out.auto_reverse_ticks = static_cast<uint16>(FMath::Clamp(In.AutoReverseTicks, 0, 65535));
		Out.current_lap = static_cast<uint16>(FMath::Clamp(In.CurrentLap, 0, 65535));
		Out.gear = static_cast<int8>(FMath::Clamp(In.Gear, -128, 127));
		Out.flags = In.Flags;
		return Out;
	}
}
