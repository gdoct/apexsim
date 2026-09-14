#pragma once

#include "CoreMinimal.h"

/**
 * Snapshot interpolation for a server-driven car.
 *
 * The server broadcasts a car's pose at the telemetry rate (60Hz by default)
 * and the client renders at whatever the GPU manages. Easing the actor
 * towards the newest sample hides the steps but never removes them: the
 * actor sprints after each new sample and slows down until the next one, so
 * its speed pumps at the broadcast rate, and it always sits behind the
 * sample by a distance that grows with speed. Frames also arrive in lumps —
 * two in one client tick, none in the next — which the chase turns straight
 * into a stutter.
 *
 * `FApexCarMotionBuffer` does what a video player does instead: it keeps the
 * last few snapshots and runs a *playhead* through them a fixed distance
 * behind the newest one, so there is nearly always a sample on each side to
 * blend between. The clock is measured in server ticks, not arrival time,
 * because the ticks are exact where the arrivals are not; the ticks-per-
 * second the playhead runs at is estimated from the arrival stream, and the
 * playhead's rate is trimmed by a small, slow correction that holds its
 * distance behind the newest sample. When a packet is late or lost the
 * playhead runs off the end and the pose is dead-reckoned from the last two
 * samples for a bounded time.
 *
 * Pure maths over plain values, so the tests can feed it any stream.
 */
namespace ApexMotion
{
	/** One telemetry sample of a car, already in the Unreal frame. */
	struct FSnapshot
	{
		int64 Tick = 0;
		FVector Location = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		/** -1..1, positive to the left as the server holds it. */
		float Steering = 0.0f;
		float SpeedMps = 0.0f;
		float EngineRpm = 0.0f;
	};

	/** What the actor should show this frame. */
	struct FPose
	{
		FVector Location = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		/** The blended motion, cm/s in the Unreal frame. */
		FVector Velocity = FVector::ZeroVector;
		float Steering = 0.0f;
		float SpeedMps = 0.0f;
		float EngineRpm = 0.0f;
		/** True when the playhead is past the newest sample and the pose is dead-reckoned. */
		bool bExtrapolated = false;
	};

	struct FSettings
	{
		/**
		 * How far behind the newest sample the playhead runs, in telemetry
		 * frames. Two frames covers the usual lump of arrivals; one leaves the
		 * playhead running off the end whenever a client tick gets no frame.
		 */
		float DelayFrames = 2.0f;

		/**
		 * How long the pose keeps moving on the last known velocity once the
		 * data runs out. Past this the car holds still until it hears more.
		 */
		float MaxExtrapolationSeconds = 0.25f;

		/**
		 * Two consecutive samples this far apart are a teleport (a respawn, a
		 * grid reset), not motion: the buffer restarts at the new place.
		 */
		float TeleportDistance = 2000.0f;

		/**
		 * The playhead's ticks per second until enough frames have arrived to
		 * measure it. The server's default tick rate.
		 */
		double DefaultTicksPerSecond = 240.0;
	};

	enum class EPushResult : uint8
	{
		/** Appended behind the newest sample. */
		Added,
		/** First sample, or a jump too large to be motion: the buffer holds only this one. */
		Restarted,
		/** Not newer than what is already held. */
		Ignored,
	};

	class APEXSIM_API FApexCarMotionBuffer
	{
	public:
		void Reset();

		/**
		 * Adds a sample. `ReceiveTimeSeconds` is the client's clock when it
		 * arrived, used only to measure the server's tick rate.
		 */
		EPushResult Push(const FSnapshot& Snapshot, double ReceiveTimeSeconds, const FSettings& Settings);

		/**
		 * Advances the playhead by `DeltaSeconds` and reads the pose there.
		 * False while nothing has been pushed.
		 */
		bool Sample(double DeltaSeconds, const FSettings& Settings, FPose& OutPose);

		/** True when a sample has arrived since the last restart. */
		bool HasData() const { return Snapshots.Num() > 0; }

		/** The playhead's clock rate, as estimated so far. */
		double GetTicksPerSecond() const { return TicksPerSecond; }

		/** Ticks between consecutive frames, as observed (the server's telemetry divisor). */
		int64 GetFrameSpacingTicks() const { return FrameSpacingTicks; }

		/** Ticks the playhead trails the newest sample by; negative while extrapolating. */
		double GetLagTicks() const;

		int32 Num() const { return Snapshots.Num(); }

	private:
		/** Oldest first, strictly increasing ticks. */
		TArray<FSnapshot> Snapshots;

		/** Ring of (tick, receive time) pairs the rate estimate is fitted to. */
		struct FTiming
		{
			int64 Tick = 0;
			double Seconds = 0.0;
		};
		TArray<FTiming> Timings;
		int32 TimingHead = 0;

		double TicksPerSecond = 0.0;
		/** False while `TicksPerSecond` is still the assumed default. */
		bool bRateMeasured = false;
		int64 FrameSpacingTicks = 0;

		double PlayheadTick = 0.0;
		bool bPlayheadValid = false;
		/** Low-passed distance between the playhead and where it should sit, in ticks. */
		double SmoothedError = 0.0;

		void RecordTiming(int64 Tick, double Seconds, const FSettings& Settings);
		void AdvancePlayhead(double DeltaSeconds, const FSettings& Settings);
		void Prune();
		double DelayTicks(const FSettings& Settings) const;
	};

	/** Blend of two samples at `Alpha` in [0, 1], rotation by the short way. */
	APEXSIM_API FPose Blend(const FSnapshot& A, const FSnapshot& B, double Alpha, double SecondsBetween);

	/** `Newest` carried on for `Seconds` at the velocity between `Previous` and `Newest`. */
	APEXSIM_API FPose Extrapolate(const FSnapshot& Previous, const FSnapshot& Newest, double SecondsBetween, double Seconds);
}
