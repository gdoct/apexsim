#include "Input/ApexFfbMarks.h"

namespace ApexFfbMarks
{
	FRecorder::FRecorder(int32 Capacity)
	{
		Ring.SetNum(FMath::Max(Capacity, 1));
	}

	void FRecorder::Push(const FSample& Sample)
	{
		Ring[Head] = Sample;
		Head = (Head + 1) % Ring.Num();
		Count = FMath::Min(Count + 1, Ring.Num());
	}

	void FRecorder::Reset()
	{
		Head = 0;
		Count = 0;
	}

	TArray<FSample> FRecorder::Window(double Seconds) const
	{
		TArray<FSample> Out;
		if (Count == 0)
		{
			return Out;
		}
		const int32 Newest = (Head - 1 + Ring.Num()) % Ring.Num();
		const double Since = Ring[Newest].Time - Seconds;
		for (int32 Back = Count - 1; Back >= 0; --Back)
		{
			const FSample& Sample = Ring[(Newest - Back + Ring.Num()) % Ring.Num()];
			if (Sample.Time >= Since)
			{
				Out.Add(Sample);
			}
		}
		return Out;
	}

	namespace
	{
		struct FColumn
		{
			const TCHAR* Name;
			float FSample::*Field;
		};

		/** The columns summed up in the log line and written to the CSV, in that order. */
		const FColumn Columns[] = {
			{ TEXT("torque"), &FSample::Torque },
			{ TEXT("constant"), &FSample::Constant },
			{ TEXT("correction"), &FSample::Correction },
			{ TEXT("stiffness_limit"), &FSample::StiffnessLimit },
			{ TEXT("road"), &FSample::Road },
			{ TEXT("vibration"), &FSample::Vibration },
			{ TEXT("vibration_hz"), &FSample::VibrationHz },
			{ TEXT("damper"), &FSample::Damper },
			{ TEXT("pad_low"), &FSample::PadLow },
			{ TEXT("pad_high"), &FSample::PadHigh },
			{ TEXT("front_slide"), &FSample::FrontSlide },
			{ TEXT("rear_slide"), &FSample::RearSlide },
			{ TEXT("front_load"), &FSample::FrontLoad },
			{ TEXT("curb"), &FSample::Curb },
			{ TEXT("off_track"), &FSample::OffTrack },
			{ TEXT("front_heat"), &FSample::FrontHeat },
			{ TEXT("suspension_damage"), &FSample::SuspensionDamage },
			{ TEXT("engine_damage"), &FSample::EngineDamage },
			{ TEXT("flat_spot"), &FSample::FlatSpot },
		};
	}

	FString Summary(int32 Number, const FString& Word, const TArray<FSample>& Samples)
	{
		FString Line = FString::Printf(TEXT("MARK %d \"%s\""), Number, *Word);
		if (Samples.IsEmpty())
		{
			return Line + TEXT(": no driving samples (not in a race, or no feedback yet)");
		}
		const FSample& Now = Samples.Last();
		Line += FString::Printf(TEXT(": %.1f s window, %d frames%s; now %.0f km/h gear %d station %.0f m steer %+.3f rim %+.1f deg"),
			Now.Time - Samples[0].Time, Samples.Num(), Now.bPredicted ? TEXT(", predicted car") : TEXT(""),
			Now.SpeedMps * 3.6f, Now.Gear, Now.StationM, Now.LocalSteer, Now.RimDegrees);
		Line += TEXT(";");
		for (const FColumn& Column : Columns)
		{
			float Sum = 0.0f;
			float Min = TNumericLimits<float>::Max();
			float Max = TNumericLimits<float>::Lowest();
			for (const FSample& Sample : Samples)
			{
				const float Value = Sample.*Column.Field;
				Sum += Value;
				Min = FMath::Min(Min, Value);
				Max = FMath::Max(Max, Value);
			}
			Line += FString::Printf(TEXT(" %s %.3f [%.3f..%.3f]"), Column.Name, Sum / Samples.Num(), Min, Max);
		}
		return Line;
	}

	FString Csv(const TArray<FSample>& Samples)
	{
		FString Out = TEXT("t_s,speed_mps,gear,station_m,steer,rim_deg,predicted");
		for (const FColumn& Column : Columns)
		{
			Out += TEXT(",");
			Out += Column.Name;
		}
		Out += TEXT("\n");
		const double End = Samples.IsEmpty() ? 0.0 : Samples.Last().Time;
		for (const FSample& Sample : Samples)
		{
			Out += FString::Printf(TEXT("%.4f,%.2f,%d,%.1f,%.4f,%.2f,%d"), Sample.Time - End, Sample.SpeedMps, Sample.Gear,
				Sample.StationM, Sample.LocalSteer, Sample.RimDegrees, Sample.bPredicted ? 1 : 0);
			for (const FColumn& Column : Columns)
			{
				Out += FString::Printf(TEXT(",%.4f"), Sample.*Column.Field);
			}
			Out += TEXT("\n");
		}
		return Out;
	}
}
