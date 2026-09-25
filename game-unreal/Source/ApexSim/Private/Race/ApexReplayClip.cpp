#include "Race/ApexReplayClip.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous: a unity build would otherwise leak `X`, `Speed`,
// `Gear`... into every file after this one, where they shadow locals.
namespace ApexReplayField
{
	enum : int32
	{
		X, Y, Z, Yaw, Pitch, Roll, Speed, Throttle, Brake, Steering, Gear, Rpm, Lap, Station, OnTrack, Finish
	};
}

namespace ApexReplayClipMath
{
	float LerpAngle(float A, float B, double Alpha)
	{
		return A + FMath::FindDeltaAngleRadians(A, B) * static_cast<float>(Alpha);
	}

	/** The lap station, blended across the line rather than back round the lap. */
	float LerpStation(float A, float B, double Alpha, float LapM)
	{
		float Delta = B - A;
		if (LapM > 0.0f && FMath::Abs(Delta) > LapM * 0.5f)
		{
			Delta += Delta < 0.0f ? LapM : -LapM;
		}
		float Out = A + Delta * static_cast<float>(Alpha);
		if (LapM > 0.0f)
		{
			Out = FMath::Fmod(Out + LapM, LapM);
		}
		return Out;
	}
}

bool FApexReplayClip::LoadFromFile(const FString& Path, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	return LoadFromString(Text, OutError);
}

bool FApexReplayClip::LoadFromString(const FString& Json, FString& OutError)
{
	*this = FApexReplayClip();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("not JSON");
		return false;
	}
	FString Format;
	if (!Root->TryGetStringField(TEXT("format"), Format) || Format != TEXT("apexsim-clip"))
	{
		OutError = TEXT("not an apexsim-clip file (cut one with `apexsim-replay cut ... --out x.clip.json`)");
		return false;
	}

	Root->TryGetStringField(TEXT("track_name"), TrackName);
	Root->TryGetStringField(TEXT("track_stem"), TrackStem);
	Root->TryGetStringField(TEXT("track_id"), TrackId);
	double Number = 0.0;
	if (Root->TryGetNumberField(TEXT("track_length_m"), Number))
	{
		TrackLengthM = static_cast<float>(Number);
	}
	if (Root->TryGetNumberField(TEXT("tick_rate"), Number))
	{
		TickRate = FMath::Max(1, FMath::RoundToInt32(Number));
	}
	if (Root->TryGetNumberField(TEXT("weather"), Number))
	{
		Conditions.Weather = static_cast<EApexWeather>(
			FMath::Clamp(static_cast<int32>(Number), 0, FApexSessionConditions::WeatherCount - 1));
	}
	if (Root->TryGetNumberField(TEXT("time_of_day_minutes"), Number))
	{
		Conditions.TimeOfDayMinutes = static_cast<int32>(Number);
	}
	Conditions = Conditions.Clamped();

	const TArray<TSharedPtr<FJsonValue>>* CarArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("cars"), CarArray) || CarArray->Num() == 0)
	{
		OutError = TEXT("the clip has no cars");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *CarArray)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object))
		{
			OutError = TEXT("a car entry is not an object");
			return false;
		}
		FCarInfo& Car = Cars.AddDefaulted_GetRef();
		Car.Index = Cars.Num() - 1;
		(*Object)->TryGetStringField(TEXT("name"), Car.Name);
		(*Object)->TryGetStringField(TEXT("car_config_id"), Car.CarConfigId);
		if ((*Object)->TryGetNumberField(TEXT("livery"), Number))
		{
			Car.Livery = static_cast<int32>(Number);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* LineArray = nullptr;
	if (Root->TryGetArrayField(TEXT("centerline"), LineArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *LineArray)
		{
			const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
			if (Value.IsValid() && Value->TryGetArray(Pair) && Pair->Num() >= 2)
			{
				Centerline.Add(FVector2D((*Pair)[0]->AsNumber(), (*Pair)[1]->AsNumber()));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* FrameArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("frames"), FrameArray) || FrameArray->Num() == 0)
	{
		OutError = TEXT("the clip has no frames");
		return false;
	}
	const int32 CarCount = Cars.Num();
	Ticks.Reserve(FrameArray->Num());
	Values.Reserve(FrameArray->Num() * CarCount * FieldsPerCar);
	for (const TSharedPtr<FJsonValue>& Value : *FrameArray)
	{
		const TSharedPtr<FJsonObject>* Frame = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Frame))
		{
			OutError = TEXT("a frame is not an object");
			return false;
		}
		double Tick = 0.0;
		(*Frame)->TryGetNumberField(TEXT("tick"), Tick);
		if (Ticks.Num() > 0 && static_cast<int64>(Tick) <= Ticks.Last())
		{
			OutError = FString::Printf(TEXT("frame ticks go backwards at %lld"), static_cast<int64>(Tick));
			return false;
		}
		double State = 2.0;
		(*Frame)->TryGetNumberField(TEXT("state"), State);
		double Countdown = -1.0;
		if (!(*Frame)->TryGetNumberField(TEXT("countdown_ms"), Countdown))
		{
			Countdown = -1.0;
		}
		const TArray<TSharedPtr<FJsonValue>>* FrameCars = nullptr;
		if (!(*Frame)->TryGetArrayField(TEXT("cars"), FrameCars) || FrameCars->Num() != CarCount)
		{
			OutError = FString::Printf(TEXT("frame at tick %lld does not list %d cars"), static_cast<int64>(Tick), CarCount);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& CarValue : *FrameCars)
		{
			const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
			if (!CarValue.IsValid() || !CarValue->TryGetArray(Fields) || Fields->Num() < FieldsPerCar)
			{
				OutError = FString::Printf(TEXT("a car at tick %lld has fewer than %d values"), static_cast<int64>(Tick), FieldsPerCar);
				return false;
			}
			for (int32 Field = 0; Field < FieldsPerCar; ++Field)
			{
				Values.Add(static_cast<float>((*Fields)[Field]->AsNumber()));
			}
		}
		Ticks.Add(static_cast<int64>(Tick));
		States.Add(static_cast<uint8>(FMath::Clamp(static_cast<int32>(State), 0, 3)));
		CountdownMs.Add(static_cast<int32>(Countdown));
	}
	return true;
}

double FApexReplayClip::GetDurationSeconds() const
{
	return Ticks.Num() > 1 ? static_cast<double>(Ticks.Last() - Ticks[0]) / TickRate : 0.0;
}

void FApexReplayClip::Locate(double Seconds, int32& OutIndex, double& OutAlpha) const
{
	OutIndex = 0;
	OutAlpha = 0.0;
	if (Ticks.Num() < 2)
	{
		return;
	}
	const double Tick = static_cast<double>(Ticks[0]) + FMath::Max(Seconds, 0.0) * TickRate;
	if (Tick >= static_cast<double>(Ticks.Last()))
	{
		OutIndex = Ticks.Num() - 1;
		return;
	}
	// Last frame at or before the tick.
	int32 Low = 0;
	int32 High = Ticks.Num() - 1;
	while (High - Low > 1)
	{
		const int32 Mid = (Low + High) / 2;
		if (static_cast<double>(Ticks[Mid]) <= Tick)
		{
			Low = Mid;
		}
		else
		{
			High = Mid;
		}
	}
	OutIndex = Low;
	const double Span = static_cast<double>(Ticks[Low + 1] - Ticks[Low]);
	OutAlpha = Span > 0.0 ? FMath::Clamp((Tick - Ticks[Low]) / Span, 0.0, 1.0) : 0.0;
}

void FApexReplayClip::SampleAt(double Seconds, FApexTelemetryFrame& OutFrame) const
{
	OutFrame = FApexTelemetryFrame();
	if (!IsValid())
	{
		return;
	}
	int32 Index = 0;
	double Alpha = 0.0;
	Locate(Seconds, Index, Alpha);
	const int32 Next = FMath::Min(Index + 1, Ticks.Num() - 1);

	OutFrame.ServerTick = Ticks[Index];
	OutFrame.SessionState = static_cast<EApexSessionState>(States[Index]);
	OutFrame.GameMode = EApexGameMode::Race;
	OutFrame.CountdownMs = CountdownMs[Index] >= 0
		? FMath::RoundToInt(FMath::Lerp(static_cast<double>(CountdownMs[Index]), static_cast<double>(FMath::Max(CountdownMs[Next], 0)), Alpha))
		: -1;
	OutFrame.Cars.Reserve(Cars.Num());
	using namespace ApexReplayField;
	using ApexReplayClipMath::LerpAngle;
	using ApexReplayClipMath::LerpStation;
	for (int32 Car = 0; Car < Cars.Num(); ++Car)
	{
		const float* A = CarValues(Index, Car);
		const float* B = CarValues(Next, Car);
		// A car jumping more than 50 m between frames was put somewhere (the
		// grid, a reset): no blend across it.
		const bool bJump = FVector2D::Distance(FVector2D(A[X], A[Y]), FVector2D(B[X], B[Y])) > 50.0f;
		const double T = bJump ? 0.0 : Alpha;
		auto Lerp = [A, B, T](int32 Field) { return FMath::Lerp(A[Field], B[Field], static_cast<float>(T)); };

		FApexCarTelemetry& Out = OutFrame.Cars.AddDefaulted_GetRef();
		Out.CarIndex = Car;
		Out.Position = FVector(Lerp(X), Lerp(Y), Lerp(Z));
		Out.YawRad = LerpAngle(A[Yaw], B[Yaw], T);
		Out.PitchRad = LerpAngle(A[Pitch], B[Pitch], T);
		Out.RollRad = LerpAngle(A[Roll], B[Roll], T);
		Out.SpeedMps = Lerp(Speed);
		Out.Throttle = Lerp(Throttle);
		Out.Brake = Lerp(Brake);
		Out.Steering = Lerp(Steering);
		Out.Gear = FMath::RoundToInt(A[Gear]);
		Out.EngineRpm = Lerp(Rpm);
		Out.CurrentLap = FMath::RoundToInt(A[Lap]);
		Out.TrackProgress = LerpStation(A[Station], B[Station], T, TrackLengthM);
		// Blended across the line, the lap is already the next one's: the
		// station and the lap must agree, or the car reads a lap behind.
		if (TrackLengthM > 0.0f && B[Station] < A[Station] - TrackLengthM * 0.5f && Out.TrackProgress < A[Station])
		{
			Out.CurrentLap = FMath::RoundToInt(B[Lap]);
		}
		Out.bIsOnTrack = A[OnTrack] > 0.5f;
		Out.FinishPosition = FMath::RoundToInt(A[Finish]);
		// The tool writes a row of zeros for a car missing from a frame.
		Out.bInGarage = IsEmptyRow(A);
	}
}

FApexSessionRoster FApexReplayClip::MakeRoster() const
{
	FApexSessionRoster Roster;
	Roster.SessionId = TEXT("replay");
	for (const FCarInfo& Car : Cars)
	{
		FApexRosterEntry& Entry = Roster.Entries.AddDefaulted_GetRef();
		Entry.CarIndex = Car.Index;
		Entry.PlayerId = FString::Printf(TEXT("replay-%d"), Car.Index);
		Entry.PlayerName = Car.Name;
		Entry.bIsAi = true;
		Entry.CarConfigId = Car.CarConfigId;
		Entry.Livery = Car.Livery;
	}
	return Roster;
}

bool FApexReplayClip::IsEmptyRow(const float* Row)
{
	for (int32 Field = 0; Field < FieldsPerCar; ++Field)
	{
		if (Row[Field] != 0.0f)
		{
			return false;
		}
	}
	return true;
}

int32 FApexReplayClip::LeaderAt(double Seconds) const
{
	FApexTelemetryFrame Frame;
	SampleAt(Seconds, Frame);
	int32 Best = INDEX_NONE;
	double BestDistance = -TNumericLimits<double>::Max();
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (Car.bInGarage)
		{
			continue;
		}
		// A finished car is behind everyone still racing for the camera.
		const double Distance = Car.FinishPosition > 0
			? -1.0e9 - Car.FinishPosition
			: static_cast<double>(Car.CurrentLap) * FMath::Max(TrackLengthM, 1.0f) + Car.TrackProgress;
		if (Distance > BestDistance)
		{
			BestDistance = Distance;
			Best = Car.CarIndex;
		}
	}
	return Best;
}

int32 FApexReplayClip::NearestCarTo(const FVector& ServerMetres, double Seconds) const
{
	FApexTelemetryFrame Frame;
	SampleAt(Seconds, Frame);
	int32 Best = INDEX_NONE;
	double BestDistance = TNumericLimits<double>::Max();
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (Car.bInGarage)
		{
			continue;
		}
		const double Distance = FVector::DistSquared(Car.Position, ServerMetres);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Car.CarIndex;
		}
	}
	return Best;
}
