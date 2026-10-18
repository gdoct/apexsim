#include "ApexTestCommon.h"
#include "ApexPredictionFixture.h"
#include "Catalog/ApexContentCrc.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Race/ApexOwnCarPredictor.h"
#include "Race/ApexPhysicsLib.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Client prediction against a recorded server sequence
 * (ApexPredictionFixture.h, written by server/ffi/tests/client_fixture.rs):
 * Monza from the YAML alone, an F1 car with aids and a tuned setup, 60 Hz
 * numbered inputs, the car's state blob at the window's start and its
 * snapshot every telemetry tick after.
 *
 * Without the physics library linked every test passes with a warning: the
 * client runs without prediction then.
 */
namespace ApexPredictionTests
{
	struct FFixtureInput
	{
		uint32 Seq = 0;
		uint32 FirstTick = 0;
		ApxInput Input = {};
	};

	struct FFixtureSnapshot
	{
		uint32 Tick = 0;
		uint32 Seq = 0;
		ApxCarSnapshot Snapshot = {};
	};

	/** The DriverFeedback the server sent at a telemetry tick (its own-car fields left out). */
	struct FFixtureFeedback
	{
		uint32 Tick = 0;
		TArray<float> SteerTorque;
		float SlipRatio[4] = {};
		float SlipAngle[4] = {};
		float SuspensionMps[4] = {};
		float ImpactMps = 0.0f;
		float SteerKick = 0.0f;
		float SteerInput = 0.0f;
		float SteerStiffness = 0.0f;
		float FrontLoad = 1.0f;
		uint8 Surface[4] = {};
		uint8 FlatSpot[4] = {};
		bool bAbs = false;
		bool bTc = false;
	};

	struct FFixture
	{
		int32 TickRateHz = 0;
		uint8 RoadContact = 0;
		uint8 GridSlot = 1;
		uint32 YamlCrc = 0;
		uint32 CarCrc = 0;
		TArray<uint8> Conditions;
		TArray<int8> Clicks;
		FString Car;
		FString TrackFile;
		uint32 BlobTick = 0;
		uint32 BlobSeq = 0;
		TArray<uint8> Blob;
		TArray<FFixtureInput> Inputs;
		TArray<FFixtureSnapshot> Snapshots;
		/** The server's DriverFeedback every telemetry tick of the window. */
		TArray<FFixtureFeedback> Feedback;

		/** The input the server held at `Tick`. */
		const FFixtureInput& InputAt(uint32 Tick) const
		{
			const FFixtureInput* Best = &Inputs[0];
			for (const FFixtureInput& One : Inputs)
			{
				if (One.FirstTick <= Tick)
				{
					Best = &One;
				}
			}
			return *Best;
		}

		const FFixtureSnapshot* SnapshotAt(uint32 Tick) const
		{
			return Snapshots.FindByPredicate([Tick](const FFixtureSnapshot& S) { return S.Tick == Tick; });
		}
	};

	/** Little-endian reads over the fixture's bytes. */
	struct FReader
	{
		TArrayView<const uint8> Data;
		int32 Pos = 0;
		bool bOk = true;

		bool Need(int32 N)
		{
			bOk = bOk && Pos + N <= Data.Num();
			return bOk;
		}
		uint8 U8() { return Need(1) ? Data[Pos++] : 0; }
		uint16 U16() { uint16 V = 0; if (Need(2)) { FMemory::Memcpy(&V, Data.GetData() + Pos, 2); Pos += 2; } return V; }
		uint32 U32() { uint32 V = 0; if (Need(4)) { FMemory::Memcpy(&V, Data.GetData() + Pos, 4); Pos += 4; } return V; }
		float F32() { float V = 0; if (Need(4)) { FMemory::Memcpy(&V, Data.GetData() + Pos, 4); Pos += 4; } return V; }
		void F32s(float* Out, int32 N) { for (int32 i = 0; i < N; ++i) { Out[i] = F32(); } }
		TArray<uint8> Bytes()
		{
			const int32 N = static_cast<int32>(U32());
			TArray<uint8> Out;
			if (Need(N))
			{
				Out.Append(Data.GetData() + Pos, N);
				Pos += N;
			}
			return Out;
		}
		FString Text()
		{
			const TArray<uint8> B = Bytes();
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(B.GetData()), B.Num());
			return FString(Converted.Length(), Converted.Get());
		}
		ApxInput Input()
		{
			ApxInput I = {};
			I.throttle = F32();
			I.brake = F32();
			I.steering = F32();
			I.clutch = F32();
			I.gear = static_cast<int8>(U8());
			I.has_gear = U8();
			I.has_clutch = U8();
			I.drs = U8();
			I.headlights = static_cast<int8>(U8());
			I.flash = U8();
			I.ers_mode = static_cast<int8>(U8());
			I.ers_boost = U8();
			I.handbrake = U8();
			return I;
		}
		ApxCarSnapshot Snapshot()
		{
			ApxCarSnapshot S = {};
			F32s(S.pos, 3);
			F32s(S.attitude, 3);
			F32s(S.vel, 3);
			S.speed_mps = F32();
			F32s(S.angular_vel, 3);
			F32s(S.wheel_angular_vel, 4);
			F32s(S.wheel_load_n, 4);
			F32s(S.suspension_travel_m, 4);
			F32s(S.suspension_velocity_mps, 4);
			F32s(S.g_forces, 3);
			S.vertical_accel_mps2 = F32();
			F32s(S.ground_slope, 2);
			F32s(S.porpoise, 2);
			F32s(S.controls, 4);
			S.engine_rpm = F32();
			S.turbo_spool = F32();
			S.track_progress = F32();
			S.lateral_offset_m = F32();
			S.centerline_idx = U32();
			S.auto_shift_hold_ticks = U16();
			S.auto_reverse_ticks = U16();
			S.current_lap = U16();
			S.gear = static_cast<int8>(U8());
			S.flags = U8();
			return S;
		}
	};

	bool ParseFixture(FFixture& Out, FString& OutError)
	{
		FReader R{TArrayView<const uint8>(ApexPredictionFixture::Bytes, sizeof(ApexPredictionFixture::Bytes))};
		if (!R.Need(4) || FMemory::Memcmp(R.Data.GetData(), "APXF", 4) != 0)
		{
			OutError = TEXT("not an APXF fixture");
			return false;
		}
		R.Pos = 4;
		if (R.U32() != 2)
		{
			OutError = TEXT("fixture version");
			return false;
		}
		Out.TickRateHz = R.U16();
		Out.RoadContact = R.U8();
		Out.GridSlot = R.U8();
		Out.YamlCrc = R.U32();
		Out.CarCrc = R.U32();
		Out.Conditions = R.Bytes();
		for (const uint8 B : R.Bytes())
		{
			Out.Clicks.Add(static_cast<int8>(B));
		}
		Out.Car = R.Text();
		Out.TrackFile = R.Text();
		Out.BlobTick = R.U32();
		Out.BlobSeq = R.U32();
		Out.Blob = R.Bytes();
		const uint32 NumInputs = R.U32();
		for (uint32 i = 0; i < NumInputs && R.bOk; ++i)
		{
			FFixtureInput In;
			In.Seq = R.U32();
			In.FirstTick = R.U32();
			In.Input = R.Input();
			Out.Inputs.Add(In);
		}
		const uint32 NumSnapshots = R.U32();
		for (uint32 i = 0; i < NumSnapshots && R.bOk; ++i)
		{
			FFixtureSnapshot S;
			S.Tick = R.U32();
			S.Seq = R.U32();
			S.Snapshot = R.Snapshot();
			Out.Snapshots.Add(S);
		}
		const uint32 NumFeedback = R.U32();
		for (uint32 i = 0; i < NumFeedback && R.bOk; ++i)
		{
			FFixtureFeedback F;
			F.Tick = R.U32();
			const uint32 Samples = R.U32();
			for (uint32 s = 0; s < Samples && R.bOk; ++s)
			{
				F.SteerTorque.Add(R.F32());
			}
			R.F32s(F.SlipRatio, 4);
			R.F32s(F.SlipAngle, 4);
			R.F32s(F.SuspensionMps, 4);
			F.ImpactMps = R.F32();
			F.SteerKick = R.F32();
			F.SteerInput = R.F32();
			F.SteerStiffness = R.F32();
			F.FrontLoad = R.F32();
			for (uint8& B : F.Surface)
			{
				B = R.U8();
			}
			for (uint8& B : F.FlatSpot)
			{
				B = R.U8();
			}
			F.bAbs = R.U8() != 0;
			F.bTc = R.U8() != 0;
			Out.Feedback.Add(MoveTemp(F));
		}
		if (!R.bOk || R.Pos != R.Data.Num() || Out.Inputs.Num() == 0 || Out.Snapshots.Num() == 0 || Out.Feedback.Num() == 0)
		{
			OutError = TEXT("the fixture is truncated or has bytes left over");
			return false;
		}
		return true;
	}

	FString ContentDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content")));
	}

	const TCHAR* Regenerate = TEXT("regenerate it: cd server/ffi && cargo test --test client_fixture -- --ignored --nocapture");

	/** The fixture's track (its YAML alone, as it was recorded) and car, through the library. */
	struct FWorld
	{
		FFixture Fixture;
		TUniquePtr<ApexPhysics::FTrack> Track;
		TArray<uint8> Toml;

		bool Load(FAutomationTestBase& Test)
		{
			FString Error;
			if (!ParseFixture(Fixture, Error))
			{
				Test.AddError(FString::Printf(TEXT("ApexPredictionFixture.h: %s; %s"), *Error, Regenerate));
				return false;
			}
			TArray<uint8> Yaml;
			const FString YamlPath = FPaths::Combine(ContentDir(), Fixture.TrackFile);
			if (!FFileHelper::LoadFileToArray(Yaml, *YamlPath))
			{
				Test.AddError(FString::Printf(TEXT("no %s"), *YamlPath));
				return false;
			}
			if (ApexContentCrc::Compute(Yaml) != Fixture.YamlCrc)
			{
				Test.AddError(FString::Printf(TEXT("%s changed since the fixture was recorded; %s"), *YamlPath, Regenerate));
				return false;
			}
			const FString TomlPath = FPaths::Combine(ContentDir(), TEXT("cars"), TEXT("default"), Fixture.Car, TEXT("car.toml"));
			if (!FFileHelper::LoadFileToArray(Toml, *TomlPath) || ApexContentCrc::Compute(Toml) != Fixture.CarCrc)
			{
				Test.AddError(FString::Printf(TEXT("%s is missing or changed since the fixture was recorded; %s"), *TomlPath, Regenerate));
				return false;
			}
			// The YAML alone in a scratch folder: no sidecars, as recorded.
			const FString Scratch = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Prediction")));
			const FString Copy = FPaths::Combine(Scratch, FPaths::GetCleanFilename(YamlPath));
			IFileManager::Get().MakeDirectory(*Scratch, true);
			if (!FFileHelper::SaveArrayToFile(Yaml, *Copy))
			{
				Test.AddError(FString::Printf(TEXT("could not write %s"), *Copy));
				return false;
			}
			Track = ApexPhysics::FTrack::Load(Copy, Fixture.RoadContact, Fixture.Conditions, Error);
			if (!Track)
			{
				Test.AddError(Error);
				return false;
			}
			return true;
		}

		TUniquePtr<ApexPhysics::FCar> NewCar(FAutomationTestBase& Test) const
		{
			FString Error;
			TUniquePtr<ApexPhysics::FCar> Car = ApexPhysics::FCar::Create(*Track, Toml, Fixture.Clicks, Fixture.GridSlot, Error);
			if (!Car)
			{
				Test.AddError(Error);
			}
			return Car;
		}
	};

	bool LibraryOrWarn(FAutomationTestBase& Test)
	{
		FString Why;
		if (!ApexPhysics::IsAvailable(&Why))
		{
			Test.AddWarning(FString::Printf(TEXT("prediction not tested: %s"), *Why));
			return false;
		}
		return true;
	}

	float DistanceM(const ApxCarPose& Pose, const ApxCarSnapshot& Snapshot)
	{
		return static_cast<float>(FVector::Dist(FVector(Pose.pos_x, Pose.pos_y, Pose.pos_z),
			FVector(Snapshot.pos[0], Snapshot.pos[1], Snapshot.pos[2])));
	}

	FApexPlayerInput ToPlayerInput(const ApxInput& I)
	{
		FApexPlayerInput P;
		P.Throttle = I.throttle;
		P.Brake = I.brake;
		P.Steering = I.steering;
		P.Gear = I.has_gear ? I.gear : -128;
		P.Clutch = I.has_clutch ? I.clutch : -1.0f;
		P.bDrs = I.drs != 0;
		P.Headlights = I.headlights;
		P.bFlash = I.flash != 0;
		P.ErsMode = I.ers_mode;
		P.bErsBoost = I.ers_boost != 0;
		P.bHandbrake = I.handbrake != 0;
		return P;
	}

	FApexOwnCarSnapshot ToWire(const FFixtureSnapshot& F)
	{
		const ApxCarSnapshot& S = F.Snapshot;
		FApexOwnCarSnapshot W;
		W.Tick = F.Tick;
		W.InputSeq = F.Seq;
		for (int32 i = 0; i < 3; ++i)
		{
			W.Pos[i] = S.pos[i];
			W.Attitude[i] = S.attitude[i];
			W.Vel[i] = S.vel[i];
			W.AngularVel[i] = S.angular_vel[i];
			W.GForces[i] = S.g_forces[i];
		}
		for (int32 i = 0; i < 4; ++i)
		{
			W.WheelAngularVel[i] = S.wheel_angular_vel[i];
			W.WheelLoadN[i] = S.wheel_load_n[i];
			W.SuspensionTravelM[i] = S.suspension_travel_m[i];
			W.SuspensionVelocityMps[i] = S.suspension_velocity_mps[i];
			W.Controls[i] = S.controls[i];
		}
		W.SpeedMps = S.speed_mps;
		W.VerticalAccelMps2 = S.vertical_accel_mps2;
		W.GroundSlope[0] = S.ground_slope[0];
		W.GroundSlope[1] = S.ground_slope[1];
		W.Porpoise[0] = S.porpoise[0];
		W.Porpoise[1] = S.porpoise[1];
		W.Gear = S.gear;
		W.EngineRpm = S.engine_rpm;
		W.TurboSpool = S.turbo_spool;
		W.AutoShiftHoldTicks = S.auto_shift_hold_ticks;
		W.AutoReverseTicks = S.auto_reverse_ticks;
		W.TrackProgress = S.track_progress;
		W.LateralOffsetM = S.lateral_offset_m;
		W.CurrentLap = S.current_lap;
		W.CenterlineIdx = S.centerline_idx == APX_NO_INDEX ? -1 : static_cast<int64>(S.centerline_idx);
		W.Flags = S.flags;
		return W;
	}

	/**
	 * The loop over the recorded sequence as a client at a ping would see it:
	 * each input sent `UpSeconds` before the tick it first drove, each
	 * snapshot heard `DownSeconds` after its tick, the blob after its slices,
	 * frames at `FrameSeconds`. Optional: drop every Nth snapshot, shift the
	 * server's car sideways from a tick on (a contact the client did not
	 * predict), or move it far (a relocation).
	 */
	struct FLoopRun
	{
		double UpSeconds = 0.04;
		double DownSeconds = 0.04;
		double FrameSeconds = 1.0 / 75.0;
		int32 DropEvery = 0;
		uint32 ShiftFromTick = 0;
		float ShiftM = 0.0f;
		uint32 RelocateFromTick = 0;

		FApexPredictionStats Stats;
		/** The predicted car at a snapshot's tick, before that snapshot was heard, against it, m. */
		TArray<float> AheadErrorsM;
		float WorstOffsetAfterShiftM = 0.0f;
		float OffsetLateAfterShiftM = 0.0f;
		float OffsetAtRelocateM = -1.0f;
		double MeanLeadTicks = 0.0;
		double AlignTicks = 0.0;
		bool bSeated = false;

		void Run(const FWorld& World, TUniquePtr<ApexPhysics::FCar> Car)
		{
			const FFixture& F = World.Fixture;
			const double Rate = F.TickRateHz;
			// Wall time of a server tick.
			auto TickSeconds = [Rate](uint32 Tick) { return 100.0 + Tick / Rate; };
			FApexOwnCarPredictor Predictor;
			Predictor.Begin(World.Track.Get(), MoveTemp(Car), F.TickRateHz);

			TArray<FApexSentInput> Sent;
			for (const FFixtureInput& In : F.Inputs)
			{
				Sent.Add({In.Seq, ToPlayerInput(In.Input), TickSeconds(In.FirstTick) - UpSeconds - 0.0005});
			}
			int32 NextSent = 0;
			int32 NextSnapshot = 0;
			int32 NextAhead = 0;
			bool bBlobSent = false;
			// The blob's slices: nine feedback messages after its tick.
			const double BlobArrives = TickSeconds(F.BlobTick) + DownSeconds + 9.0 / 60.0;
			const double End = TickSeconds(F.Snapshots.Last().Tick) + DownSeconds;
			double LeadSum = 0.0;
			int32 LeadFrames = 0;
			for (double Now = TickSeconds(F.BlobTick); Now <= End; Now += FrameSeconds)
			{
				TArray<FApexSentInput> New;
				while (NextSent < Sent.Num() && Sent[NextSent].SentSeconds <= Now)
				{
					New.Add(Sent[NextSent++]);
				}
				Predictor.AddSentInputs(New, Now);
				if (!bBlobSent && BlobArrives <= Now)
				{
					Predictor.OnStateBlob(F.BlobTick, F.Blob, Now);
					bBlobSent = true;
				}
				// The net subsystem keeps the newest snapshot a frame brought,
				// with when the UDP worker took it in.
				const FFixtureSnapshot* Newest = nullptr;
				while (NextSnapshot < F.Snapshots.Num() && TickSeconds(F.Snapshots[NextSnapshot].Tick) + DownSeconds <= Now)
				{
					const bool bDropped = DropEvery > 0 && (NextSnapshot % DropEvery) == DropEvery - 1;
					if (!bDropped)
					{
						Newest = &F.Snapshots[NextSnapshot];
					}
					++NextSnapshot;
				}
				if (Newest)
				{
					FApexOwnCarSnapshot Wire = ToWire(*Newest);
					if (ShiftFromTick > 0 && Newest->Tick >= ShiftFromTick)
					{
						Wire.Pos[1] += ShiftM;
					}
					if (RelocateFromTick > 0 && Newest->Tick >= RelocateFromTick)
					{
						Wire.Pos[0] += 300.0f;
					}
					Predictor.OnSnapshot(Wire, TickSeconds(Newest->Tick) + DownSeconds);
				}
				Predictor.Advance(Now, FrameSeconds);
				if (Predictor.IsSeated())
				{
					LeadSum += Predictor.GetClockTick() - (Now - 100.0) * Rate;
					++LeadFrames;
					// Each snapshot tick the head has passed, as it was first drawn,
					// against the server's car there, when that is still to be heard.
					while (NextAhead < F.Snapshots.Num() && F.Snapshots[NextAhead].Tick <= Predictor.GetHeadTick())
					{
						const FFixtureSnapshot& Truth = F.Snapshots[NextAhead++];
						ApxCarPose Drawn;
						if (TickSeconds(Truth.Tick) + DownSeconds > Now && ShiftFromTick == 0 && RelocateFromTick == 0
							&& Predictor.GetPoseAtTick(Truth.Tick, Drawn))
						{
							AheadErrorsM.Add(DistanceM(Drawn, Truth.Snapshot));
						}
					}
				}
				if (ShiftFromTick > 0)
				{
					const double SinceShift = Now - (TickSeconds(ShiftFromTick) + DownSeconds);
					if (SinceShift >= 0.0)
					{
						WorstOffsetAfterShiftM = FMath::Max(WorstOffsetAfterShiftM, Predictor.GetCorrectionOffsetM());
					}
					if (SinceShift >= 0.4 && SinceShift < 0.4 + FrameSeconds)
					{
						OffsetLateAfterShiftM = Predictor.GetCorrectionOffsetM();
					}
				}
				if (RelocateFromTick > 0 && OffsetAtRelocateM < 0.0f
					&& Now >= TickSeconds(RelocateFromTick) + DownSeconds + 2.0 * FrameSeconds)
				{
					OffsetAtRelocateM = Predictor.GetCorrectionOffsetM();
				}
			}
			bSeated = bSeated || Predictor.IsSeated();
			MeanLeadTicks = LeadFrames > 0 ? LeadSum / LeadFrames : 0.0;
			AlignTicks = Predictor.GetAlignTicks();
			Stats = Predictor.TakeStats();
		}
	};
}

using namespace ApexPredictionTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPredictionLibraryTest, "ApexSim.Prediction.Library", ApexTestFlags)

bool FApexPredictionLibraryTest::RunTest(const FString& Parameters)
{
	FString Why;
	const bool bAvailable = ApexPhysics::IsAvailable(&Why);
#if WITH_APEX_PHYSICS
	TestTrue(FString::Printf(TEXT("the linked library speaks this ABI (%s)"), *Why), bAvailable);
#else
	TestFalse(TEXT("built without the library, prediction says so"), bAvailable);
	TestFalse(TEXT("and why"), Why.IsEmpty());
#endif
	FFixture Fixture;
	FString Error;
	TestTrue(FString::Printf(TEXT("the fixture parses (%s)"), *Error), ParseFixture(Fixture, Error));
	TestEqual(TEXT("the fixture's tick rate"), Fixture.TickRateHz, 420);
	TestEqual(TEXT("31 setup knobs"), Fixture.Clicks.Num(), 31);
	TestTrue(TEXT("snapshots every telemetry tick"), Fixture.Snapshots.Num() > 100);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPredictionReplayTest, "ApexSim.Prediction.ReplaysTheServer", ApexTestFlags)

bool FApexPredictionReplayTest::RunTest(const FString& Parameters)
{
	if (!LibraryOrWarn(*this))
	{
		return true;
	}
	FWorld World;
	if (!World.Load(*this))
	{
		return false;
	}
	TUniquePtr<ApexPhysics::FCar> Car = World.NewCar(*this);
	if (!Car)
	{
		return false;
	}
	const FFixture& F = World.Fixture;
	TestTrue(TEXT("the server's blob loads"), Car->Load(F.Blob));

	// The server's inputs from its blob: the library lands on every snapshot.
	float Worst = 0.0f;
	int32 Compared = 0;
	ApxCarPose Pose;
	for (uint32 Tick = F.BlobTick + 1; Tick <= F.Snapshots.Last().Tick; ++Tick)
	{
		if (!Car->Step(*World.Track, F.InputAt(Tick).Input, Tick, F.TickRateHz))
		{
			AddError(FString::Printf(TEXT("step %u: %s"), Tick, *ApexPhysics::LastError()));
			return false;
		}
		if (const FFixtureSnapshot* S = F.SnapshotAt(Tick))
		{
			Car->Pose(Pose);
			Worst = FMath::Max(Worst, DistanceM(Pose, S->Snapshot));
			++Compared;
		}
	}
	AddInfo(FString::Printf(TEXT("%d snapshots over %.1f s: the library is at most %.2f mm from the server (its road as loaded)"),
		Compared, static_cast<double>(F.Snapshots.Last().Tick - F.BlobTick) / F.TickRateHz, Worst * 1000.0f));
	TestEqual(TEXT("every snapshot compared"), Compared, F.Snapshots.Num());
	TestTrue(FString::Printf(TEXT("within 5 mm of the server (%.2f mm); %s"), Worst * 1000.0f, Regenerate), Worst < 0.005f);

	// A snapshot reads back as laid.
	ApxCarSnapshot Back;
	TestTrue(TEXT("snapshot apply"), Car->ApplySnapshot(F.Snapshots[3].Snapshot));
	TestTrue(TEXT("snapshot read"), Car->Snapshot(Back));
	TestEqual(TEXT("laid position"), Back.pos[0], F.Snapshots[3].Snapshot.pos[0]);
	TestEqual(TEXT("laid wire round trip"), ApexPhysics::ToSnapshot(ToWire(F.Snapshots[3])).pos[1], F.Snapshots[3].Snapshot.pos[1]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPredictionRewindTest, "ApexSim.Prediction.SnapshotRewind", ApexTestFlags)

bool FApexPredictionRewindTest::RunTest(const FString& Parameters)
{
	if (!LibraryOrWarn(*this))
	{
		return true;
	}
	FWorld World;
	if (!World.Load(*this))
	{
		return false;
	}
	const FFixture& F = World.Fixture;
	// A car whose own state went wrong (stepped on the brakes instead of the
	// server's inputs), the server's snapshot laid over it at a tick, and the
	// server's inputs replayed a ping's worth (42 ticks): where it lands
	// against the server's next snapshot, with and without the snapshot.
	ApxInput Wrong = {};
	Wrong.brake = 1.0f;
	Wrong.headlights = -1;
	Wrong.ers_mode = -1;
	float WorstWith = 0.0f;
	float BestWithout = TNumericLimits<float>::Max();
	int32 Probes = 0;
	for (int32 k = 6; k + 6 < F.Snapshots.Num(); k += 12)
	{
		const FFixtureSnapshot& At = F.Snapshots[k];
		const FFixtureSnapshot* Later = F.SnapshotAt(At.Tick + 42);
		if (!Later)
		{
			continue;
		}
		for (const bool bLay : {true, false})
		{
			TUniquePtr<ApexPhysics::FCar> Car = World.NewCar(*this);
			if (!Car || !Car->Load(F.Blob))
			{
				return false;
			}
			for (uint32 Tick = F.BlobTick + 1; Tick <= At.Tick; ++Tick)
			{
				Car->Step(*World.Track, Wrong, Tick, F.TickRateHz);
			}
			if (bLay)
			{
				Car->ApplySnapshot(At.Snapshot);
			}
			for (uint32 Tick = At.Tick + 1; Tick <= Later->Tick; ++Tick)
			{
				Car->Step(*World.Track, F.InputAt(Tick).Input, Tick, F.TickRateHz);
			}
			ApxCarPose Pose;
			Car->Pose(Pose);
			const float Error = DistanceM(Pose, Later->Snapshot);
			if (bLay)
			{
				WorstWith = FMath::Max(WorstWith, Error);
				++Probes;
			}
			else
			{
				BestWithout = FMath::Min(BestWithout, Error);
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d rewinds over a wrong state: the snapshot lands within %.2f mm after 42 ticks; without it at best %.2f m"),
		Probes, WorstWith * 1000.0f, BestWithout));
	TestTrue(TEXT("several probes"), Probes >= 4);
	// The slow state the snapshot leaves out (tyre and brake heat after the
	// wrong braking) is what is left: millimetres.
	TestTrue(FString::Printf(TEXT("a snapshot over a wrong state replays within 10 mm (%.2f mm)"), WorstWith * 1000.0f), WorstWith < 0.01f);
	TestTrue(TEXT("the wrong state alone is far off"), BestWithout > 0.1f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPredictionLoopTest, "ApexSim.Prediction.Loop.AtPing", ApexTestFlags)

bool FApexPredictionLoopTest::RunTest(const FString& Parameters)
{
	if (!LibraryOrWarn(*this))
	{
		return true;
	}
	FWorld World;
	if (!World.Load(*this))
	{
		return false;
	}
	for (const double RttMs : {0.0, 80.0, 160.0})
	{
		FLoopRun Run;
		Run.UpSeconds = Run.DownSeconds = RttMs / 2000.0;
		Run.Run(World, World.NewCar(*this));
		const FApexPredictionStats& S = Run.Stats;
		float AheadWorst = 0.0f;
		for (const float E : Run.AheadErrorsM)
		{
			AheadWorst = FMath::Max(AheadWorst, E);
		}
		AddInfo(FString::Printf(TEXT("%.0f ms: %s; ahead of the server %d times, at worst %.2f mm off its later snapshot; align %+.1f ticks"),
			RttMs, *S.Describe(World.Fixture.TickRateHz), Run.AheadErrorsM.Num(), AheadWorst * 1000.0f, Run.AlignTicks));
		const FString At = FString::Printf(TEXT(" at %.0f ms"), RttMs);
		TestTrue(TEXT("seated") + At, Run.bSeated);
		// With no ping the timeline sits on the server's tick: most snapshots
		// land at or past the head, which steps on to them instead of rewinding.
		TestTrue(TEXT("rewinds on the snapshots") + At, RttMs == 0.0 || S.Rewinds > World.Fixture.Snapshots.Num() / 2);
		TestTrue(FString::Printf(TEXT("corrections stay small (p95 %.2f mm)%s"), S.CorrectionPercentileMm(0.95f), *At),
			S.CorrectionPercentileMm(0.95f) < 5.0f);
		TestTrue(FString::Printf(TEXT("the largest correction is small (%.2f mm)%s"), S.MaxCorrectionMm(), *At), S.MaxCorrectionMm() < 20.0f);
		TestEqual(TEXT("no re-seats") + At, S.Reseats, 0);
		// The replay reaches back a round trip and the input's wait.
		const float WantDepth = static_cast<float>(RttMs / 1000.0 * World.Fixture.TickRateHz);
		TestTrue(FString::Printf(TEXT("replay depth %.1f ticks follows the ping (%.1f)%s"), S.MeanReplayTicks(), WantDepth, *At),
			S.MeanReplayTicks() > WantDepth - 4.0f && S.MeanReplayTicks() < WantDepth + 20.0f);
		if (RttMs > 0.0)
		{
			TestTrue(TEXT("the car was predicted ahead of the server") + At, Run.AheadErrorsM.Num() > 10);
			// Millimetres, growing with how far ahead it runs (a brake stab is
			// timed to within a tick of the server's).
			const float AheadLimitMm = FMath::Max(10.0f, static_cast<float>(RttMs) / 8.0f);
			TestTrue(FString::Printf(TEXT("ahead of the server within %.0f mm (%.2f mm)%s"), AheadLimitMm, AheadWorst * 1000.0f, *At),
				AheadWorst * 1000.0f < AheadLimitMm);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPredictionLossTest, "ApexSim.Prediction.Loop.Loss", ApexTestFlags)

bool FApexPredictionLossTest::RunTest(const FString& Parameters)
{
	if (!LibraryOrWarn(*this))
	{
		return true;
	}
	FWorld World;
	if (!World.Load(*this))
	{
		return false;
	}
	// Every third snapshot lost: the next one rewinds further and still lands.
	FLoopRun Run;
	Run.DropEvery = 3;
	Run.Run(World, World.NewCar(*this));
	AddInfo(Run.Stats.Describe(World.Fixture.TickRateHz));
	TestTrue(TEXT("rewinds on what arrives"), Run.Stats.Rewinds > World.Fixture.Snapshots.Num() / 3);
	TestTrue(FString::Printf(TEXT("corrections stay small (max %.2f mm)"), Run.Stats.MaxCorrectionMm()), Run.Stats.MaxCorrectionMm() < 20.0f);
	TestEqual(TEXT("no re-seats"), Run.Stats.Reseats, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPredictionContactTest, "ApexSim.Prediction.Loop.Contact", ApexTestFlags)

bool FApexPredictionContactTest::RunTest(const FString& Parameters)
{
	if (!LibraryOrWarn(*this))
	{
		return true;
	}
	FWorld World;
	if (!World.Load(*this))
	{
		return false;
	}
	// The server's car pushed 0.3 m sideways (a hit the client did not
	// predict): one large correction, drawn as a blend that has nearly gone
	// 0.4 s later.
	FLoopRun Run;
	Run.ShiftFromTick = World.Fixture.Snapshots[World.Fixture.Snapshots.Num() / 2].Tick;
	Run.ShiftM = 0.3f;
	Run.Run(World, World.NewCar(*this));
	AddInfo(FString::Printf(TEXT("%s; offset at most %.0f mm, %.1f mm 0.4 s on"), *Run.Stats.Describe(World.Fixture.TickRateHz),
		Run.WorstOffsetAfterShiftM * 1000.0f, Run.OffsetLateAfterShiftM * 1000.0f));
	TestTrue(FString::Printf(TEXT("the hit is one correction of about its size (%.0f mm)"), Run.Stats.MaxCorrectionMm()),
		Run.Stats.MaxCorrectionMm() > 250.0f && Run.Stats.MaxCorrectionMm() < 400.0f);
	TestTrue(TEXT("and not a re-seat"), Run.Stats.Reseats == 0);
	TestTrue(TEXT("drawn as a blend"), Run.WorstOffsetAfterShiftM > 0.1f);
	TestTrue(FString::Printf(TEXT("that has nearly gone 0.4 s on (%.1f mm)"), Run.OffsetLateAfterShiftM * 1000.0f),
		Run.OffsetLateAfterShiftM < 0.03f);
	TestTrue(TEXT("one large correction only"), Run.Stats.CorrectionPercentileMm(0.9f) < 20.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPredictionRelocationTest, "ApexSim.Prediction.Loop.Relocation", ApexTestFlags)

bool FApexPredictionRelocationTest::RunTest(const FString& Parameters)
{
	if (!LibraryOrWarn(*this))
	{
		return true;
	}
	FWorld World;
	if (!World.Load(*this))
	{
		return false;
	}
	// The server put the car 300 m on (a recovery, a relocation): the
	// prediction goes there at once, no blend across the circuit.
	FLoopRun Run;
	Run.RelocateFromTick = World.Fixture.Snapshots[World.Fixture.Snapshots.Num() / 2].Tick;
	Run.Run(World, World.NewCar(*this));
	AddInfo(Run.Stats.Describe(World.Fixture.TickRateHz));
	TestTrue(TEXT("re-seated"), Run.Stats.Reseats >= 1);
	TestTrue(FString::Printf(TEXT("with no blend across it (%.1f mm left)"), Run.OffsetAtRelocateM * 1000.0f),
		Run.OffsetAtRelocateM >= 0.0f && Run.OffsetAtRelocateM < 0.5f);
	return true;
}

#endif
