#include "Race/ApexOwnCarPredictor.h"

#include "ApexSim.h"

namespace
{
	float WrapPi(float A)
	{
		return FMath::UnwindRadians(A);
	}

	FVector PoseAttitude(const ApxCarPose& P)
	{
		return FVector(P.yaw_rad, P.pitch_rad, P.roll_rad);
	}

	FVector PosePosition(const ApxCarPose& P)
	{
		return FVector(P.pos_x, P.pos_y, P.pos_z);
	}
}

// --- Stats ---------------------------------------------------------------------

float FApexPredictionStats::MeanCorrectionMm() const
{
	if (CorrectionsMm.Num() == 0)
	{
		return 0.0f;
	}
	double Sum = 0.0;
	for (const float Mm : CorrectionsMm)
	{
		Sum += Mm;
	}
	return static_cast<float>(Sum / CorrectionsMm.Num());
}

float FApexPredictionStats::CorrectionPercentileMm(float P) const
{
	if (CorrectionsMm.Num() == 0)
	{
		return 0.0f;
	}
	TArray<float> Sorted = CorrectionsMm;
	Sorted.Sort();
	const int32 Index = FMath::Clamp(FMath::CeilToInt(P * Sorted.Num()) - 1, 0, Sorted.Num() - 1);
	return Sorted[Index];
}

float FApexPredictionStats::MaxCorrectionMm() const
{
	float Max = 0.0f;
	for (const float Mm : CorrectionsMm)
	{
		Max = FMath::Max(Max, Mm);
	}
	return Max;
}

FString FApexPredictionStats::Describe(int32 TickRateHz) const
{
	const float Rate = static_cast<float>(FMath::Max(TickRateHz, 1));
	return FString::Printf(
		TEXT("%d snapshots, %d rewinds (%d from a whole state), replay depth mean %.1f max %d ticks, ")
		TEXT("correction mean %.2f p50 %.2f p95 %.2f max %.2f mm, %d re-seats, %d clock jumps, %d stale, lead %.1f ticks (%.0f ms)"),
		Snapshots, Rewinds, BlobLoads, MeanReplayTicks(), MaxReplayTicks, MeanCorrectionMm(),
		CorrectionPercentileMm(0.5f), CorrectionPercentileMm(0.95f), MaxCorrectionMm(), Reseats, ClockSnaps, Stale,
		MeanLeadTicks(), MeanLeadTicks() / Rate * 1000.0f);
}

// --- Timeline ------------------------------------------------------------------

void FApexOwnCarPredictor::Begin(ApexPhysics::FTrack* InTrack, TUniquePtr<ApexPhysics::FCar> InCar, int32 InTickRateHz)
{
	End();
	Track = InTrack;
	Car = MoveTemp(InCar);
	TickRateHz = FMath::Max(InTickRateHz, 1);
	Ring.SetNum(FMath::Max(64, FMath::CeilToInt(Settings.RingSeconds * TickRateHz)));
}

void FApexOwnCarPredictor::End()
{
	Car.Reset();
	Track = nullptr;
	Ring.Reset();
	bSeated = false;
	Head = -1;
	SeatTick = -1;
	Clock = 0.0;
	ClockSeconds = 0.0;
	DirtyFrom = -1;
	bServerCorrection = false;
	Inputs.Reset();
	LastInput = ApxInput{};
	LastSeq = 0;
	bHaveSnapshot = false;
	SnapshotSpacing = 1;
	PreviousSnapshotTick = -1;
	SpacingAge = 0;
	LastSnapshotSeq = -1;
	bServerDriven = false;
	ServerClock.Reset();
	RoundTrips.Reset();
	RttSeconds = -1.0;
	AlignTicks = 0.0;
	bHaveServerClock = false;
	OffsetPos = FVector::ZeroVector;
	OffsetAttitude = FVector::ZeroVector;
}

FApexPredictionStats FApexOwnCarPredictor::TakeStats()
{
	FApexPredictionStats Out = MoveTemp(Stats);
	Stats = FApexPredictionStats();
	return Out;
}

const FApexOwnCarPredictor::FEntry* FApexOwnCarPredictor::FindEntry(int64 Tick) const
{
	if (Ring.Num() == 0 || Tick < 0)
	{
		return nullptr;
	}
	const FEntry& Entry = Ring[static_cast<int32>(Tick % Ring.Num())];
	return Entry.Tick == Tick ? &Entry : nullptr;
}

int64 FApexOwnCarPredictor::OldestTick() const
{
	return FMath::Max(SeatTick, Head - Ring.Num() + 1);
}

bool FApexOwnCarPredictor::IsSaveTick(int64 Tick) const
{
	// The full state is kept where a snapshot can land: the server sends one
	// every telemetry tick, on a fixed phase.
	if (SnapshotSpacing <= 1 || !bHaveSnapshot)
	{
		return true;
	}
	const int64 Phase = Latest.Tick % SnapshotSpacing;
	return (Tick % SnapshotSpacing) == Phase;
}

void FApexOwnCarPredictor::Trim(TArray<FTimingSample>& Samples, double NowSeconds) const
{
	int32 Drop = 0;
	while (Drop < Samples.Num() && Samples[Drop].Seconds < NowSeconds - Settings.TimingWindowSeconds)
	{
		++Drop;
	}
	if (Drop > 0)
	{
		Samples.RemoveAt(0, Drop, EAllowShrinking::No);
	}
}

double FApexOwnCarPredictor::ServerTickNow(double NowSeconds) const
{
	// The earliest arrival of the window is the least delayed: its tick less
	// its arrival is the server's clock, a one-way trip behind.
	double Base = -DBL_MAX;
	for (const FTimingSample& Sample : ServerClock)
	{
		Base = FMath::Max(Base, Sample.Value);
	}
	return Base + NowSeconds * TickRateHz;
}

double FApexOwnCarPredictor::TargetClock(double NowSeconds) const
{
	const double Rtt = RttSeconds >= 0.0 && !bServerDriven ? RttSeconds : FallbackRttSeconds;
	return ServerTickNow(NowSeconds) + Rtt * TickRateHz + Settings.LeadMarginTicks + (bServerDriven ? 0.0 : AlignTicks);
}

double FApexOwnCarPredictor::GetLeadTicks(double NowSeconds) const
{
	return bHaveServerClock ? Clock - ServerTickNow(NowSeconds) : 0.0;
}

// --- Input ---------------------------------------------------------------------

void FApexOwnCarPredictor::AddSentInputs(TArrayView<const FApexSentInput> Sent, double NowSeconds)
{
	for (const FApexSentInput& One : Sent)
	{
		if (One.Seq <= LastSeq)
		{
			continue;
		}
		FInputRecord Record;
		Record.Seq = One.Seq;
		Record.Input = ApexPhysics::ToInput(One.Input);
		Record.SentSeconds = One.SentSeconds;
		Inputs.Add(Record);
		LastSeq = One.Seq;
		LastInput = Record.Input;
	}
	// Kept as long as the ring reaches, and the round trip's window.
	const double KeepSeconds = FMath::Max(Settings.RingSeconds, Settings.TimingWindowSeconds) + 1.0;
	int32 Drop = 0;
	while (Drop + 1 < Inputs.Num() && Inputs[Drop].SentSeconds < NowSeconds - KeepSeconds)
	{
		++Drop;
	}
	if (Drop > 0)
	{
		Inputs.RemoveAt(0, Drop, EAllowShrinking::No);
	}
	PlaceInputs(false);
}

void FApexOwnCarPredictor::PlaceInputs(bool bAll)
{
	// On the timeline where the clock stood when each went out. Placed once,
	// unless the clock was (re)set: a seat, or a jump.
	double Floor = -DBL_MAX;
	for (FInputRecord& Record : Inputs)
	{
		const bool bNew = !Record.bPlaced;
		if (bAll || bNew)
		{
			Record.FromTick = Clock - (ClockSeconds - Record.SentSeconds) * TickRateHz;
			Record.bPlaced = bSeated;
		}
		Record.FromTick = FMath::Max(Record.FromTick, Floor);
		Floor = Record.FromTick;
		if (bNew && Record.bPlaced && !bAll)
		{
			// Heard after the timeline stepped past where it belongs (inputs
			// reach this loop once a frame): those ticks take it, and step again.
			const int64 First = FMath::Max(static_cast<int64>(FMath::CeilToDouble(Record.FromTick)), OldestTick() + 1);
			for (int64 Tick = First; Tick <= Head; ++Tick)
			{
				FEntry& Entry = EntryAt(Tick);
				if (Entry.Tick == Tick && !Entry.bExact && Entry.Seq < Record.Seq)
				{
					Entry.Input = Record.Input;
					Entry.Seq = Record.Seq;
					DirtyFrom = DirtyFrom < 0 ? Tick - 1 : FMath::Min(DirtyFrom, Tick - 1);
				}
			}
		}
	}
}

ApxInput FApexOwnCarPredictor::HeldServerInput() const
{
	// What the server last applied, held: the replay's best guess at a
	// driver it cannot hear.
	ApxInput Input = {};
	Input.throttle = Latest.Controls[0];
	Input.brake = Latest.Controls[1];
	Input.steering = Latest.Controls[2];
	Input.has_gear = 1;
	Input.gear = static_cast<int8>(FMath::Clamp(Latest.Gear, -1, 127));
	Input.drs = (Latest.Flags & FApexOwnCarSnapshot::FlagDrsOpen) ? 1 : 0;
	Input.headlights = -1;
	Input.ers_mode = -1;
	return Input;
}

ApxInput FApexOwnCarPredictor::InputForTick(int64 Tick, uint32& OutSeq) const
{
	if (bServerDriven && bHaveSnapshot)
	{
		OutSeq = 0;
		return HeldServerInput();
	}
	// The newest input sent by the time the clock reached this tick.
	for (int32 i = Inputs.Num() - 1; i >= 0; --i)
	{
		if (Inputs[i].FromTick <= static_cast<double>(Tick))
		{
			OutSeq = Inputs[i].Seq;
			return Inputs[i].Input;
		}
	}
	if (Inputs.Num() > 0)
	{
		OutSeq = Inputs[0].Seq;
		return Inputs[0].Input;
	}
	OutSeq = 0;
	return LastInput;
}

// --- Server states ---------------------------------------------------------------

void FApexOwnCarPredictor::Record(FEntry& Entry, int64 Tick)
{
	Entry.Tick = Tick;
	Car->Pose(Entry.Pose);
	Entry.bExact = false;
	Entry.bHasState = IsSaveTick(Tick) && Car->Save(Entry.State);
}

bool FApexOwnCarPredictor::StepTick(int64 Tick, bool bReplay)
{
	FEntry& Entry = EntryAt(Tick);
	if (bReplay && Entry.Tick == Tick && Entry.bExact)
	{
		// The server's own state of this tick: nothing to step.
		if (!Car->Load(Entry.State))
		{
			return false;
		}
		Car->Pose(Entry.Pose);
	}
	else
	{
		ApxInput Input;
		uint32 Seq = 0;
		if (bReplay && Entry.Tick == Tick && !bServerDriven)
		{
			Input = Entry.Input;
			Seq = Entry.Seq;
		}
		else
		{
			Input = InputForTick(Tick, Seq);
		}
		if (!Car->Step(*Track, Input, static_cast<uint32>(Tick), TickRateHz))
		{
			return false;
		}
		if (bHaveSnapshot && Latest.Tick == Tick)
		{
			Car->ApplySnapshot(LatestApx);
		}
		Record(Entry, Tick);
		Entry.Input = Input;
		Entry.Seq = Seq;
	}
	if (Tick > Head)
	{
		Head = Tick;
	}
	return true;
}

void FApexOwnCarPredictor::Seat(int64 Tick, TArrayView<const uint8> Blob, bool bFromBlob, double NowSeconds)
{
	if (!Car->Load(Blob))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Prediction: the server's state of tick %lld did not load: %s"), Tick,
			*ApexPhysics::LastError());
		return;
	}
	for (FEntry& Entry : Ring)
	{
		Entry.Tick = -1;
		Entry.bHasState = false;
		Entry.bExact = false;
	}
	FEntry& Entry = EntryAt(Tick);
	Entry.Tick = Tick;
	Car->Pose(Entry.Pose);
	Entry.State = TArray<uint8>(Blob);
	Entry.bHasState = true;
	Entry.bExact = bFromBlob;
	Entry.Input = LastInput;
	Entry.Seq = 0;
	Head = Tick;
	SeatTick = Tick;
	DirtyFrom = -1;
	bServerCorrection = false;
	Clock = bHaveServerClock ? FMath::Max(TargetClock(NowSeconds), static_cast<double>(Tick)) : static_cast<double>(Tick);
	bSeated = true;
	ClockSeconds = NowSeconds;
	PlaceInputs(true);
	OffsetPos = FVector::ZeroVector;
	OffsetAttitude = FVector::ZeroVector;
	bSeated = true;
}

void FApexOwnCarPredictor::OnStateBlob(int64 Tick, TArrayView<const uint8> Blob, double NowSeconds)
{
	if (!IsRunning() || Tick < 0 || Blob.Num() == 0)
	{
		return;
	}
	if (!bSeated || Tick > Head || Tick < OldestTick())
	{
		// The first, or one the timeline has not reached (it trails the
		// server) or no longer holds: the timeline starts again on it.
		if (bSeated && bCounting)
		{
			++Stats.Reseats;
		}
		Seat(Tick, Blob, true, NowSeconds);
		return;
	}
	FEntry& Entry = EntryAt(Tick);
	Entry.Tick = Tick;
	Entry.State = TArray<uint8>(Blob);
	Entry.bHasState = true;
	Entry.bExact = true;
	DirtyFrom = DirtyFrom < 0 ? Tick : FMath::Min(DirtyFrom, Tick);
	bServerCorrection = true;
	if (bCounting)
	{
		++Stats.BlobLoads;
	}
}

void FApexOwnCarPredictor::OnSnapshot(const FApexOwnCarSnapshot& Snapshot, double NowSeconds)
{
	if (!IsRunning() || Snapshot.Tick < 0 || (bHaveSnapshot && Snapshot.Tick <= Latest.Tick))
	{
		return;
	}
	if (bCounting)
	{
		++Stats.Snapshots;
	}

	// Where the snapshots land: every telemetry tick, so the spacing is the
	// smallest gap seen lately (a lost one doubles a gap, never halves it).
	if (PreviousSnapshotTick >= 0)
	{
		const int64 Gap = Snapshot.Tick - PreviousSnapshotTick;
		if (Gap > 0 && Gap <= 64 && (Gap < SnapshotSpacing || SnapshotSpacing <= 1 || ++SpacingAge > 120))
		{
			SnapshotSpacing = Gap;
			SpacingAge = 0;
		}
	}
	PreviousSnapshotTick = Snapshot.Tick;

	// The server clock: the window's earliest arrival.
	Trim(ServerClock, NowSeconds);
	ServerClock.Add({NowSeconds, static_cast<double>(Snapshot.Tick) - NowSeconds * TickRateHz});
	bHaveServerClock = true;

	// Whose inputs drive the car.
	if (Snapshot.InputSeq != LastSnapshotSeq)
	{
		LastSnapshotSeq = Snapshot.InputSeq;
		LastSeqChangeSeconds = NowSeconds;
	}
	bServerDriven = Snapshot.InputSeq < 0
		|| (NowSeconds - LastSeqChangeSeconds > Settings.ServerDrivenSeconds && LastSeq > static_cast<uint32>(Snapshot.InputSeq));

	// The round trip: the input the snapshot names had reached the server by
	// its tick and the next had not, so the trip is between hearing the
	// snapshot less the next's sending and less its own. The middle of that,
	// the shortest of the window (the least delayed hearing).
	if (Snapshot.InputSeq > 0 && !bServerDriven)
	{
		for (int32 i = Inputs.Num() - 2; i >= 0; --i)
		{
			if (Inputs[i].Seq == static_cast<uint32>(Snapshot.InputSeq))
			{
				const double Longest = NowSeconds - Inputs[i].SentSeconds;
				const double Shortest = NowSeconds - Inputs[i + 1].SentSeconds;
				Trim(RoundTrips, NowSeconds);
				RoundTrips.Add({NowSeconds, FMath::Max(0.0, 0.5 * (Longest + Shortest))});
				double Min = DBL_MAX;
				for (const FTimingSample& Sample : RoundTrips)
				{
					Min = FMath::Min(Min, Sample.Value);
				}
				RttSeconds = Min;
				break;
			}
		}
	}

	Latest = Snapshot;
	LatestApx = ApexPhysics::ToSnapshot(Snapshot);
	bHaveSnapshot = true;
	if (bCounting && bSeated)
	{
		Stats.LeadTicksSum += Clock - static_cast<double>(Snapshot.Tick);
		++Stats.LeadSamples;
	}

	if (!bSeated)
	{
		return;
	}
	if (Snapshot.Tick > Head && Snapshot.Tick - Head <= Ring.Num() / 2)
	{
		// The timeline has not stepped that far yet (just seated, or the
		// clock lagged): step it there, the snapshot laid on its tick.
		while (Head < Snapshot.Tick)
		{
			if (!StepTick(Head + 1, false))
			{
				break;
			}
		}
		return;
	}
	if (Snapshot.Tick > Head)
	{
		// Far ahead of the timeline: the head's state with the snapshot laid
		// over it starts it again there.
		Car->ApplySnapshot(LatestApx);
		TArray<uint8> Blob;
		if (Car->Save(Blob))
		{
			if (bCounting)
			{
				++Stats.Reseats;
			}
			Seat(Snapshot.Tick, Blob, false, NowSeconds);
		}
		return;
	}
	if (Snapshot.Tick < OldestTick())
	{
		if (bCounting)
		{
			++Stats.Stale;
		}
		return;
	}
	AlignInputs(Snapshot);
	DirtyFrom = DirtyFrom < 0 ? Snapshot.Tick : FMath::Min(DirtyFrom, Snapshot.Tick);
	bServerCorrection = true;
}

void FApexOwnCarPredictor::AlignInputs(const FApexOwnCarSnapshot& Snapshot)
{
	// The snapshot says which input the server held at its tick; the ring
	// says which this timeline did. A different one means the inputs land on
	// the timeline early or late against the server's: the clock's target
	// moves by part of the gap, until they agree.
	const FEntry* Entry = FindEntry(Snapshot.Tick);
	if (bServerDriven || Snapshot.InputSeq <= 0 || !Entry || Entry->Seq == 0)
	{
		return;
	}
	const uint32 Served = static_cast<uint32>(Snapshot.InputSeq);
	auto FromTickOf = [this](uint32 Seq) -> double
	{
		for (const FInputRecord& Record : Inputs)
		{
			if (Record.Seq == Seq)
			{
				return Record.FromTick;
			}
		}
		return -1.0;
	};
	double Gap = 0.0;
	if (Entry->Seq < Served)
	{
		// Late: the server held the input before this timeline reached it.
		const double From = FromTickOf(Served);
		Gap = From >= 0.0 ? -(From - static_cast<double>(Snapshot.Tick) + 0.5) : 0.0;
	}
	else if (Entry->Seq > Served)
	{
		// Early: this timeline moved on to the next while the server had not.
		const double From = FromTickOf(Served + 1);
		Gap = From >= 0.0 ? static_cast<double>(Snapshot.Tick) - From + 0.5 : 0.0;
	}
	const double Limit = Settings.MaxAlignTicks;
	AlignTicks = FMath::Clamp(AlignTicks + Settings.AlignGain * Gap, -Limit, Limit);
}

void FApexOwnCarPredictor::Reconcile()
{
	if (DirtyFrom < 0)
	{
		return;
	}
	const int64 Start = DirtyFrom;
	DirtyFrom = -1;
	// A replay for an input heard late is the prediction, not a correction.
	const bool bCount = bCounting && bServerCorrection;
	bServerCorrection = false;
	const FEntry* HeadEntry = FindEntry(Head);
	if (!HeadEntry || Start > Head || Start < OldestTick())
	{
		return;
	}
	const ApxCarPose Before = HeadEntry->Pose;

	// A state at or before the start: the server's, or one kept at a save tick.
	int64 From = Start;
	while (From >= OldestTick())
	{
		const FEntry* Entry = FindEntry(From);
		if (Entry && Entry->bHasState)
		{
			break;
		}
		--From;
	}
	if (From < OldestTick())
	{
		return;
	}
	FEntry& FromEntry = EntryAt(From);
	if (!Car->Load(FromEntry.State))
	{
		return;
	}
	if (bHaveSnapshot && Latest.Tick == From && !FromEntry.bExact)
	{
		Car->ApplySnapshot(LatestApx);
		Record(FromEntry, From);
	}
	const int64 OldHead = Head;
	for (int64 Tick = From + 1; Tick <= OldHead; ++Tick)
	{
		if (!StepTick(Tick, true))
		{
			break;
		}
	}
	Head = OldHead;

	const FEntry* After = FindEntry(Head);
	if (!After)
	{
		return;
	}
	const FVector Moved = PosePosition(Before) - PosePosition(After->Pose);
	const float ErrorM = static_cast<float>(Moved.Size());
	if (ErrorM > Settings.ReseatDistanceM)
	{
		// Not a misprediction: the car was put somewhere (a recovery, a
		// relocation). Go there.
		OffsetPos = FVector::ZeroVector;
		OffsetAttitude = FVector::ZeroVector;
		if (bCount)
		{
			++Stats.Reseats;
		}
	}
	else
	{
		OffsetPos += Moved;
		const FVector Turned = PoseAttitude(Before) - PoseAttitude(After->Pose);
		OffsetAttitude += FVector(WrapPi(Turned.X), WrapPi(Turned.Y), WrapPi(Turned.Z));
	}
	if (bCount)
	{
		++Stats.Rewinds;
		const int32 Depth = static_cast<int32>(Head - Start);
		Stats.ReplayTicks += Depth;
		Stats.MaxReplayTicks = FMath::Max(Stats.MaxReplayTicks, Depth);
		Stats.CorrectionsMm.Add(ErrorM * 1000.0f);
	}
}

// --- Per frame -------------------------------------------------------------------

void FApexOwnCarPredictor::Advance(double NowSeconds, double DeltaSeconds)
{
	if (!IsRunning() || !bSeated)
	{
		return;
	}
	Reconcile();

	const double Rate = static_cast<double>(TickRateHz);
	Clock += FMath::Max(DeltaSeconds, 0.0) * Rate;
	ClockSeconds = NowSeconds;
	if (bHaveServerClock)
	{
		const double Error = TargetClock(NowSeconds) - Clock;
		if (FMath::Abs(Error) > Settings.ClockSnapSeconds * Rate)
		{
			Clock += Error;
			PlaceInputs(true);
			if (bCounting)
			{
				++Stats.ClockSnaps;
			}
		}
		else
		{
			const double Trim = Error * FMath::Min(1.0, DeltaSeconds / FMath::Max(Settings.ClockGainSeconds, 0.01f));
			const double MaxTrim = Settings.ClockSlew * DeltaSeconds * Rate;
			Clock += FMath::Clamp(Trim, -MaxTrim, MaxTrim);
		}
	}

	// Never more than half a second of steps in one frame.
	const int64 Want = static_cast<int64>(FMath::CeilToDouble(Clock));
	int32 Budget = TickRateHz / 2;
	while (Head < Want && Budget-- > 0)
	{
		if (!StepTick(Head + 1, false))
		{
			break;
		}
	}
	if (Head < Want)
	{
		Clock = static_cast<double>(Head);
	}

	const float Decay = FMath::Exp(-static_cast<float>(DeltaSeconds) / FMath::Max(Settings.CorrectionSeconds, 0.001f));
	OffsetPos *= Decay;
	OffsetAttitude *= Decay;
}

bool FApexOwnCarPredictor::GetRawPose(FApexPredictedPose& Out) const
{
	const FEntry* Now = bSeated ? FindEntry(Head) : nullptr;
	if (!Now)
	{
		return false;
	}
	const FEntry* Before = FindEntry(Head - 1);
	// The head is the tick at or past the clock: blend back from it by the remainder.
	const float Alpha = Before ? FMath::Clamp(static_cast<float>(Clock - static_cast<double>(Head - 1)), 0.0f, 1.0f) : 1.0f;
	const ApxCarPose& B = Now->Pose;
	const ApxCarPose& A = Before ? Before->Pose : Now->Pose;
	Out.PositionM = FMath::Lerp(PosePosition(A), PosePosition(B), static_cast<double>(Alpha));
	Out.YawRad = A.yaw_rad + WrapPi(B.yaw_rad - A.yaw_rad) * Alpha;
	Out.PitchRad = FMath::Lerp(A.pitch_rad, B.pitch_rad, Alpha);
	Out.RollRad = FMath::Lerp(A.roll_rad, B.roll_rad, Alpha);
	Out.VelocityMps = FMath::Lerp(FVector(A.vel_x, A.vel_y, A.vel_z), FVector(B.vel_x, B.vel_y, B.vel_z), static_cast<double>(Alpha));
	Out.SpeedMps = FMath::Lerp(A.speed_mps, B.speed_mps, Alpha);
	Out.Steering = FMath::Lerp(A.steering_input, B.steering_input, Alpha);
	Out.EngineRpm = B.engine_rpm;
	Out.Gear = B.gear;
	return true;
}

bool FApexOwnCarPredictor::GetPoseAtTick(int64 Tick, ApxCarPose& Out) const
{
	const FEntry* Entry = bSeated && Tick <= Head ? FindEntry(Tick) : nullptr;
	if (!Entry)
	{
		return false;
	}
	Out = Entry->Pose;
	return true;
}

bool FApexOwnCarPredictor::GetPose(FApexPredictedPose& Out) const
{
	if (!GetRawPose(Out))
	{
		return false;
	}
	Out.PositionM += OffsetPos;
	Out.YawRad = WrapPi(Out.YawRad + static_cast<float>(OffsetAttitude.X));
	Out.PitchRad += static_cast<float>(OffsetAttitude.Y);
	Out.RollRad += static_cast<float>(OffsetAttitude.Z);
	return true;
}
