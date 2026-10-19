#pragma once

#include "CoreMinimal.h"

/**
 * Marks for a wheel sitting (docs/wheel-sitting.md): the driver presses
 * `Mark` (or runs `apexsim.debug.Mark <word>`) when something feels wrong,
 * and the last couple of seconds of what the mixers did go to the log and to
 * a CSV beside it, so a finding is a moment plus a word.
 *
 * Pure: the controller pushes one sample a frame and asks for the window.
 */
namespace ApexFfbMarks
{
	/** One frame of what the driver felt and why. */
	struct FSample
	{
		/** `FPlatformTime::Seconds()`. */
		double Time = 0.0;
		float SpeedMps = 0.0f;
		int32 Gear = 0;
		float StationM = 0.0f;
		/** Steering sent (screen sign) and where the rim is, degrees. */
		float LocalSteer = 0.0f;
		float RimDegrees = 0.0f;
		/** The server's (or the predicted car's) steering torque, 1.0 = reference. */
		float Torque = 0.0f;
		/** What went to the wheelbase. */
		float Constant = 0.0f;
		float Correction = 0.0f;
		float StiffnessLimit = 0.0f;
		float Road = 0.0f;
		float Vibration = 0.0f;
		float VibrationHz = 0.0f;
		float Damper = 0.0f;
		/** What went to a pad's motors. */
		float PadLow = 0.0f;
		float PadHigh = 0.0f;
		/** The signals behind it. */
		float FrontSlide = 0.0f;
		float RearSlide = 0.0f;
		float FrontLoad = 1.0f;
		float Curb = 0.0f;
		float OffTrack = 0.0f;
		float FrontHeat = 0.0f;
		float SuspensionDamage = 0.0f;
		float EngineDamage = 0.0f;
		float FlatSpot = 0.0f;
		bool bPredicted = false;
	};

	/** Seconds of samples a mark keeps. */
	inline constexpr double WindowSeconds = 2.0;

	/** The last few seconds of samples, oldest dropped first. */
	class APEXSIM_API FRecorder
	{
	public:
		explicit FRecorder(int32 Capacity = 1024);

		void Push(const FSample& Sample);
		void Reset();

		/** The samples within `Seconds` of the newest, oldest first. */
		TArray<FSample> Window(double Seconds = WindowSeconds) const;

	private:
		TArray<FSample> Ring;
		int32 Head = 0;
		int32 Count = 0;
	};

	/** One log line: the mark's word and number, the newest frame, and each column's mean and extremes over the window. */
	APEXSIM_API FString Summary(int32 Number, const FString& Word, const TArray<FSample>& Samples);

	/** The window as CSV, a header and one row a frame; time is seconds before the mark (negative). */
	APEXSIM_API FString Csv(const TArray<FSample>& Samples);
}
