#include "Race/ApexCarMotion.h"

namespace ApexMotion
{
	namespace
	{
		/** Samples kept behind the playhead; more is memory for nothing. */
		constexpr int32 MaxSnapshots = 32;

		/**
		 * Timing pairs the rate is fitted over: two seconds of 60Hz frames.
		 * Arrival times are quantised to client ticks, so the fit needs a
		 * span long enough for that noise to wash out.
		 */
		constexpr int32 MaxTimings = 120;

		/**
		 * The fit is trusted once it spans this long. A least-squares slope
		 * over a second of frames puts 5 ms of arrival noise well under a
		 * percent; over a quarter of a second it was several, and the
		 * playhead ran off the end of the data while the trim caught up.
		 */
		constexpr double MinTimingSpanSeconds = 1.0;

		/** No server ticks outside this; anything else is a broken fit. */
		constexpr double MinTicksPerSecond = 20.0;
		constexpr double MaxTicksPerSecond = 2000.0;

		/**
		 * The first measured rate differing from the assumed one by more
		 * than this re-seats the playhead: it has been running at the wrong
		 * speed for a second, and slewing that back would take longer than
		 * a cut.
		 */
		constexpr double RateSurpriseFraction = 0.1;

		/** Time constant of the playhead's error filter. */
		constexpr double ErrorFilterSeconds = 0.4;

		/**
		 * How long the playhead takes to close a steady offset from where it
		 * should sit. Together with the filter this is a second-order loop,
		 * and it is critically damped when this is four times the filter's
		 * time constant; any quicker and a re-seat or a hiccup rings for
		 * seconds as a visible speed wobble.
		 */
		constexpr double CorrectionSeconds = 4.0 * ErrorFilterSeconds;

		/** The most the clock is ever trimmed by, as a fraction of its rate. */
		constexpr double MaxSlew = 0.08;

		/** Beyond this the playhead is re-seated rather than slewed. */
		constexpr double ResyncSeconds = 1.0;

		FVector VelocityBetween(const FSnapshot& A, const FSnapshot& B, double SecondsBetween)
		{
			return SecondsBetween > UE_DOUBLE_SMALL_NUMBER ? (B.Location - A.Location) / SecondsBetween : FVector::ZeroVector;
		}
	}

	FPose Blend(const FSnapshot& A, const FSnapshot& B, double Alpha, double SecondsBetween)
	{
		const double T = FMath::Clamp(Alpha, 0.0, 1.0);
		FPose Pose;
		Pose.Location = FMath::Lerp(A.Location, B.Location, T);
		Pose.Rotation = FQuat::Slerp(A.Rotation, B.Rotation, static_cast<float>(T)).GetNormalized();
		Pose.Velocity = VelocityBetween(A, B, SecondsBetween);
		Pose.Steering = FMath::Lerp(A.Steering, B.Steering, static_cast<float>(T));
		Pose.SpeedMps = FMath::Lerp(A.SpeedMps, B.SpeedMps, static_cast<float>(T));
		Pose.EngineRpm = FMath::Lerp(A.EngineRpm, B.EngineRpm, static_cast<float>(T));
		Pose.bExtrapolated = false;
		return Pose;
	}

	FPose Extrapolate(const FSnapshot& Previous, const FSnapshot& Newest, double SecondsBetween, double Seconds)
	{
		FPose Pose;
		Pose.Velocity = VelocityBetween(Previous, Newest, SecondsBetween);
		Pose.Location = Newest.Location + Pose.Velocity * Seconds;
		Pose.Rotation = Newest.Rotation;
		if (SecondsBetween > UE_DOUBLE_SMALL_NUMBER)
		{
			// The turn between the last two samples, carried on at the same
			// rate: a car mid-corner keeps turning rather than freezing its
			// heading while it slides on.
			FQuat Delta = Newest.Rotation * Previous.Rotation.Inverse();
			Delta.EnforceShortestArcWith(FQuat::Identity);
			FVector Axis;
			float Angle;
			Delta.ToAxisAndAngle(Axis, Angle);
			const float Turn = Angle * static_cast<float>(Seconds / SecondsBetween);
			Pose.Rotation = (FQuat(Axis, Turn) * Newest.Rotation).GetNormalized();
		}
		Pose.Steering = Newest.Steering;
		Pose.SpeedMps = Newest.SpeedMps;
		Pose.EngineRpm = Newest.EngineRpm;
		Pose.bExtrapolated = true;
		return Pose;
	}

	void FApexCarMotionBuffer::Reset()
	{
		Snapshots.Reset();
		Timings.Reset();
		TimingHead = 0;
		TicksPerSecond = 0.0;
		bRateMeasured = false;
		FrameSpacingTicks = 0;
		PlayheadTick = 0.0;
		bPlayheadValid = false;
		SmoothedError = 0.0;
	}

	double FApexCarMotionBuffer::DelayTicks(const FSettings& Settings) const
	{
		// Before two frames have shown the spacing, assume the server default
		// (240Hz ticks broadcast at 60Hz).
		const double Spacing = FrameSpacingTicks > 0 ? static_cast<double>(FrameSpacingTicks) : 4.0;
		return FMath::Max(Settings.DelayFrames, 0.0f) * Spacing;
	}

	double FApexCarMotionBuffer::GetLagTicks() const
	{
		return Snapshots.Num() > 0 && bPlayheadValid ? static_cast<double>(Snapshots.Last().Tick) - PlayheadTick : 0.0;
	}

	void FApexCarMotionBuffer::RecordTiming(int64 Tick, double Seconds, const FSettings& Settings)
	{
		if (Timings.Num() < MaxTimings)
		{
			Timings.Add({Tick, Seconds});
		}
		else
		{
			Timings[TimingHead] = {Tick, Seconds};
			TimingHead = (TimingHead + 1) % MaxTimings;
		}

		if (TicksPerSecond <= 0.0)
		{
			TicksPerSecond = Settings.DefaultTicksPerSecond;
		}

		// The oldest pair is the one the head points at once the ring is full.
		const FTiming& Oldest = Timings.Num() < MaxTimings ? Timings[0] : Timings[TimingHead];
		if (Seconds - Oldest.Seconds < MinTimingSpanSeconds || Timings.Num() < 3)
		{
			return;
		}

		// Least-squares slope of tick against arrival time, about the means
		// so the sums stay small.
		double MeanSeconds = 0.0;
		double MeanTicks = 0.0;
		for (const FTiming& T : Timings)
		{
			MeanSeconds += T.Seconds - Oldest.Seconds;
			MeanTicks += static_cast<double>(T.Tick - Oldest.Tick);
		}
		MeanSeconds /= Timings.Num();
		MeanTicks /= Timings.Num();
		double Sxx = 0.0;
		double Sxy = 0.0;
		for (const FTiming& T : Timings)
		{
			const double Dx = (T.Seconds - Oldest.Seconds) - MeanSeconds;
			const double Dy = static_cast<double>(T.Tick - Oldest.Tick) - MeanTicks;
			Sxx += Dx * Dx;
			Sxy += Dx * Dy;
		}
		if (Sxx <= UE_DOUBLE_SMALL_NUMBER)
		{
			return;
		}
		const double Measured = Sxy / Sxx;
		if (Measured < MinTicksPerSecond || Measured > MaxTicksPerSecond)
		{
			return;
		}

		if (!bRateMeasured && FMath::Abs(Measured - TicksPerSecond) > RateSurpriseFraction * Measured)
		{
			bPlayheadValid = false;
		}
		bRateMeasured = true;
		TicksPerSecond = Measured;
	}

	EPushResult FApexCarMotionBuffer::Push(const FSnapshot& Snapshot, double ReceiveTimeSeconds, const FSettings& Settings)
	{
		if (Snapshots.Num() > 0)
		{
			const FSnapshot& Newest = Snapshots.Last();
			if (Snapshot.Tick <= Newest.Tick)
			{
				// Reordered or duplicated on the way here; the newer one is
				// already in. A tick that went a long way backwards is a new
				// session, which restarts below.
				const double Rate = TicksPerSecond > 0.0 ? TicksPerSecond : Settings.DefaultTicksPerSecond;
				if (static_cast<double>(Newest.Tick - Snapshot.Tick) < Rate * ResyncSeconds)
				{
					return EPushResult::Ignored;
				}
			}
			else if (FVector::Dist(Snapshot.Location, Newest.Location) <= Settings.TeleportDistance)
			{
				const int64 Gap = Snapshot.Tick - Newest.Tick;
				FrameSpacingTicks = FrameSpacingTicks > 0 ? FMath::Min(FrameSpacingTicks, Gap) : Gap;
				Snapshots.Add(Snapshot);
				if (Snapshots.Num() > MaxSnapshots)
				{
					Snapshots.RemoveAt(0, Snapshots.Num() - MaxSnapshots, EAllowShrinking::No);
				}
				RecordTiming(Snapshot.Tick, ReceiveTimeSeconds, Settings);
				return EPushResult::Added;
			}
		}

		// First sample, a teleport or a new session: what was held describes
		// somewhere else. The rate estimate survives a teleport (the clock
		// did not change) but not a tick that ran backwards.
		const bool bClockChanged = Snapshots.Num() > 0 && Snapshot.Tick <= Snapshots.Last().Tick;
		Snapshots.Reset();
		Snapshots.Add(Snapshot);
		bPlayheadValid = false;
		SmoothedError = 0.0;
		if (bClockChanged)
		{
			Timings.Reset();
			TimingHead = 0;
			TicksPerSecond = 0.0;
			bRateMeasured = false;
			FrameSpacingTicks = 0;
		}
		RecordTiming(Snapshot.Tick, ReceiveTimeSeconds, Settings);
		return EPushResult::Restarted;
	}

	void FApexCarMotionBuffer::AdvancePlayhead(double DeltaSeconds, const FSettings& Settings)
	{
		const double NewestTick = static_cast<double>(Snapshots.Last().Tick);
		const double TargetTick = NewestTick - DelayTicks(Settings);
		const double Rate = TicksPerSecond > 0.0 ? TicksPerSecond : Settings.DefaultTicksPerSecond;

		if (!bPlayheadValid)
		{
			PlayheadTick = TargetTick;
			bPlayheadValid = true;
			SmoothedError = 0.0;
			return;
		}

		const double Error = TargetTick - PlayheadTick;
		if (FMath::Abs(Error) > Rate * ResyncSeconds)
		{
			// A stall long past what extrapolation covers, or a clock that
			// jumped: creeping back over it would take seconds, and the car
			// has nothing true to show meanwhile.
			PlayheadTick = TargetTick;
			SmoothedError = 0.0;
			return;
		}

		// Filter before trimming: the raw error swings by a frame as the
		// arrivals lump and spread, and following that would put the pumping
		// back that the buffer exists to remove.
		const double Alpha = 1.0 - FMath::Exp(-DeltaSeconds / ErrorFilterSeconds);
		SmoothedError += (Error - SmoothedError) * Alpha;
		const double Slew = FMath::Clamp(SmoothedError / (Rate * CorrectionSeconds), -MaxSlew, MaxSlew);

		PlayheadTick += DeltaSeconds * Rate * (1.0 + Slew);

		// Nothing to show past the extrapolation limit: hold there so the
		// pose freezes instead of flying on, and so the next real frame is
		// close by when it comes.
		const double Limit = NewestTick + Settings.MaxExtrapolationSeconds * Rate;
		PlayheadTick = FMath::Min(PlayheadTick, Limit);
	}

	void FApexCarMotionBuffer::Prune()
	{
		// Keep the sample just behind the playhead (it is the blend's start)
		// and one more, which is what extrapolation needs when the buffer
		// runs dry.
		int32 Drop = 0;
		while (Snapshots.Num() - Drop > 2 && static_cast<double>(Snapshots[Drop + 1].Tick) <= PlayheadTick)
		{
			++Drop;
		}
		if (Drop > 0)
		{
			Snapshots.RemoveAt(0, Drop, EAllowShrinking::No);
		}
	}

	bool FApexCarMotionBuffer::Sample(double DeltaSeconds, const FSettings& Settings, FPose& OutPose)
	{
		if (Snapshots.Num() == 0)
		{
			return false;
		}

		AdvancePlayhead(FMath::Max(DeltaSeconds, 0.0), Settings);
		Prune();

		const double Rate = TicksPerSecond > 0.0 ? TicksPerSecond : Settings.DefaultTicksPerSecond;
		const FSnapshot& Newest = Snapshots.Last();

		if (Snapshots.Num() == 1)
		{
			// One sample says where, not how fast: sit on it.
			OutPose = Blend(Newest, Newest, 0.0, 0.0);
			return true;
		}

		if (PlayheadTick >= static_cast<double>(Newest.Tick))
		{
			const FSnapshot& Previous = Snapshots[Snapshots.Num() - 2];
			const double Between = static_cast<double>(Newest.Tick - Previous.Tick) / Rate;
			const double Ahead = (PlayheadTick - static_cast<double>(Newest.Tick)) / Rate;
			OutPose = Extrapolate(Previous, Newest, Between, Ahead);
			return true;
		}

		// The pair the playhead sits between. Prune left the start sample at
		// index 0 whenever the playhead is inside the buffer; the search is
		// for the rare frame it is not.
		int32 Index = 0;
		while (Index + 2 < Snapshots.Num() && static_cast<double>(Snapshots[Index + 1].Tick) <= PlayheadTick)
		{
			++Index;
		}
		const FSnapshot& A = Snapshots[Index];
		const FSnapshot& B = Snapshots[Index + 1];
		const double Span = static_cast<double>(B.Tick - A.Tick);
		const double Alpha = Span > 0.0 ? (PlayheadTick - static_cast<double>(A.Tick)) / Span : 1.0;
		OutPose = Blend(A, B, Alpha, Span / Rate);
		return true;
	}
}
