#include "Guide/ApexTrackGuide.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous: this module is unity-built.
namespace ApexTrackGuideJson
{
	/** The newest guide format this client reads. A newer file is still read, with what it shares. */
	constexpr int32 KnownVersion = 1;

	double Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double Default = 0.0)
	{
		double Value = Default;
		if (Object.IsValid() && Object->TryGetNumberField(Key, Value) && FMath::IsFinite(Value))
		{
			return Value;
		}
		return Default;
	}

	/** A number that may be absent or null: false then. */
	bool OptionalNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Key) : nullptr;
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			return false;
		}
		Out = Value->AsNumber();
		return FMath::IsFinite(Out);
	}

	FString String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Key, Value);
		}
		return Value.TrimStartAndEnd();
	}

	bool Bool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, bool bDefault = false)
	{
		bool bValue = bDefault;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Key, bValue);
		}
		return bValue;
	}

	TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Object.IsValid() && Object->TryGetObjectField(Key, Found) && Found)
		{
			return *Found;
		}
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Key, Found))
		{
			return Found;
		}
		return nullptr;
	}

	/** Every non-empty string of an array of strings; anything else in it is skipped. */
	TArray<FString> Lines(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		TArray<FString> Out;
		if (const TArray<TSharedPtr<FJsonValue>>* Values = Array(Object, Key))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text))
				{
					Text.TrimStartAndEndInline();
					if (!Text.IsEmpty())
					{
						Out.Add(Text);
					}
				}
			}
		}
		return Out;
	}

	/** `[x, y, z]`. */
	bool Point(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = Array(Object, Key);
		if (!Values || Values->Num() != 3)
		{
			return false;
		}
		double Xyz[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Axis];
			if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()))
			{
				return false;
			}
			Xyz[Axis] = Value->AsNumber();
		}
		Out = FVector(Xyz[0], Xyz[1], Xyz[2]);
		return true;
	}

	TArray<FApexGuideCamera> Cameras(const TSharedPtr<FJsonObject>& Object)
	{
		TArray<FApexGuideCamera> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = Array(Object, TEXT("cameras"));
		if (!Values)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry)
			{
				continue;
			}
			FApexGuideCamera Camera;
			Camera.Name = String(*Entry, TEXT("name"));
			Camera.Kind = String(*Entry, TEXT("kind")).ToLower();
			if (Camera.Kind.IsEmpty())
			{
				Camera.Kind = TEXT("fixed");
			}
			// Car-relative cameras are the client's; a tripod needs both points.
			if (Camera.Kind != TEXT("fixed") || !Point(*Entry, TEXT("eye"), Camera.EyeM) || !Point(*Entry, TEXT("look"), Camera.LookM))
			{
				continue;
			}
			Camera.FovDeg = FMath::Clamp(static_cast<float>(Number(*Entry, TEXT("fov_deg"), 50.0)), 5.0f, 120.0f);
			if (Camera.Name.IsEmpty())
			{
				Camera.Name = TEXT("Trackside");
			}
			Out.Add(MoveTemp(Camera));
		}
		return Out;
	}

	/** A clip in order: from before to, the slow part inside the loop and before its end. */
	void TidyClip(FApexGuideCorner& Corner)
	{
		if (Corner.ToS < Corner.FromS)
		{
			Swap(Corner.FromS, Corner.ToS);
		}
		Corner.FromS = FMath::Max(Corner.FromS, 0.0);
		Corner.ToS = FMath::Max(Corner.ToS, Corner.FromS);
		if (Corner.SlowToS < Corner.SlowFromS)
		{
			Swap(Corner.SlowFromS, Corner.SlowToS);
		}
		Corner.SlowFromS = FMath::Clamp(Corner.SlowFromS, Corner.FromS, Corner.ToS);
		Corner.SlowToS = FMath::Clamp(Corner.SlowToS, Corner.SlowFromS, Corner.ToS);
		Corner.SlowRate = FMath::Clamp(Corner.SlowRate, 0.05f, 1.0f);
	}
}

bool ApexTrackGuide::Parse(const FString& Json, FApexTrackGuide& Out, FString& OutError)
{
	using namespace ApexTrackGuideJson;
	Out = FApexTrackGuide();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("not JSON");
		return false;
	}

	double Version = 0.0;
	if (!OptionalNumber(Root, TEXT("version"), Version) || Version < 1.0)
	{
		OutError = TEXT("no version: not a track guide");
		return false;
	}
	Out.Version = static_cast<int32>(Version);
	Out.Recording = String(Root, TEXT("recording"));
	if (Out.Recording.IsEmpty())
	{
		OutError = TEXT("the guide names no recording");
		return false;
	}

	Out.Class = String(Root, TEXT("class"));
	Out.DisplayClass = String(Root, TEXT("display_class"));
	if (Out.DisplayClass.IsEmpty())
	{
		Out.DisplayClass = Out.Class;
	}
	Out.SourceCrc = static_cast<int64>(Number(Root, TEXT("source_crc")));
	Out.CarGapS = static_cast<float>(Number(Root, TEXT("car_gap_s")));
	if (const TSharedPtr<FJsonObject> Car = Child(Root, TEXT("car")))
	{
		Out.CarId = String(Car, TEXT("id"));
		Out.CarFolder = String(Car, TEXT("folder"));
		Out.CarName = String(Car, TEXT("name"));
	}

	if (const TSharedPtr<FJsonObject> Track = Child(Root, TEXT("track")))
	{
		FApexGuideTrack& T = Out.Track;
		T.Stem = String(Track, TEXT("stem"));
		T.TrackId = String(Track, TEXT("track_id"));
		T.DisplayName = String(Track, TEXT("display_name"));
		T.Description = String(Track, TEXT("description"));
		T.Country = String(Track, TEXT("country"));
		T.City = String(Track, TEXT("city"));
		T.Category = String(Track, TEXT("category"));
		T.YearBuilt = static_cast<int32>(Number(Track, TEXT("year_built")));
		T.LengthM = static_cast<float>(Number(Track, TEXT("length_m")));
		double Altitude = 0.0;
		T.bHasAltitude = OptionalNumber(Track, TEXT("altitude_m"), Altitude);
		T.AltitudeM = static_cast<float>(Altitude);
		T.ElevationMinM = static_cast<float>(Number(Track, TEXT("elevation_min_m")));
		T.ElevationMaxM = static_cast<float>(Number(Track, TEXT("elevation_max_m")));
		T.ClimbM = static_cast<float>(Number(Track, TEXT("climb_m")));
		T.Corners = static_cast<int32>(Number(Track, TEXT("corners")));
		T.Left = static_cast<int32>(Number(Track, TEXT("left")));
		T.Right = static_cast<int32>(Number(Track, TEXT("right")));
		T.Direction = String(Track, TEXT("direction")).ToLower();
		T.LongestStraightM = static_cast<float>(Number(Track, TEXT("longest_straight_m")));
		T.DrsZones = static_cast<int32>(Number(Track, TEXT("drs_zones")));
		T.LapTimeS = static_cast<float>(Number(Track, TEXT("lap_time_s")));
		T.TopSpeedKph = static_cast<float>(Number(Track, TEXT("top_speed_kph")));
		T.Notes = Lines(Track, TEXT("notes"));
	}

	if (const TSharedPtr<FJsonObject> Overview = Child(Root, TEXT("overview")))
	{
		Out.OverviewTimeS = FMath::Max(0.0, Number(Overview, TEXT("time_s")));
		Out.OverviewCameras = Cameras(Overview);
	}

	if (const TArray<TSharedPtr<FJsonValue>>* Corners = Array(Root, TEXT("corners")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Corners)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& C = *Entry;
			FApexGuideCorner& Corner = Out.Corners.AddDefaulted_GetRef();
			Corner.Number = static_cast<int32>(Number(C, TEXT("number"), Out.Corners.Num()));
			Corner.TurnFrom = static_cast<int32>(Number(C, TEXT("turn_from"), Corner.Number));
			Corner.TurnTo = FMath::Max(Corner.TurnFrom, static_cast<int32>(Number(C, TEXT("turn_to"), Corner.TurnFrom)));
			Corner.Name = String(C, TEXT("name"));
			if (Corner.Name.IsEmpty())
			{
				Corner.Name = Corner.TurnTo > Corner.TurnFrom ? FString::Printf(TEXT("Turns %d-%d"), Corner.TurnFrom, Corner.TurnTo)
															  : FString::Printf(TEXT("Turn %d"), Corner.TurnFrom);
			}
			Corner.Direction = String(C, TEXT("direction")).ToLower();
			Corner.EntryM = static_cast<float>(Number(C, TEXT("entry_m")));
			Corner.ApexM = static_cast<float>(Number(C, TEXT("apex_m")));
			Corner.ExitM = static_cast<float>(Number(C, TEXT("exit_m")));
			Corner.TurnDeg = static_cast<float>(Number(C, TEXT("turn_deg")));
			Corner.MinRadiusM = static_cast<float>(Number(C, TEXT("min_radius_m")));

			if (const TSharedPtr<FJsonObject> Clip = Child(C, TEXT("clip")))
			{
				Corner.FromS = Number(Clip, TEXT("from_s"));
				Corner.ToS = Number(Clip, TEXT("to_s"), Corner.FromS);
				Corner.SlowFromS = Number(Clip, TEXT("slow_from_s"), Corner.FromS);
				Corner.SlowToS = Number(Clip, TEXT("slow_to_s"), Corner.SlowFromS);
				Corner.SlowRate = static_cast<float>(Number(Clip, TEXT("slow_rate"), 0.25));
				Corner.ApexS = Number(Clip, TEXT("apex_s"), -1.0);
			}
			TidyClip(Corner);

			Corner.MinSpeedKph = static_cast<float>(Number(C, TEXT("min_speed_kph")));
			Corner.ApexGear = static_cast<int32>(Number(C, TEXT("apex_gear")));
			Corner.EntrySpeedKph = static_cast<float>(Number(C, TEXT("entry_speed_kph")));
			Corner.ExitSpeedKph = static_cast<float>(Number(C, TEXT("exit_speed_kph")));
			double Brake = 0.0;
			Corner.bHasBrake = OptionalNumber(C, TEXT("brake_m"), Brake);
			Corner.BrakeM = static_cast<float>(Brake);
			Corner.bFlatOut = Bool(C, TEXT("flat_out"), false);
			Corner.ElevationChangeM = static_cast<float>(Number(C, TEXT("elevation_change_m")));
			Corner.BankingDeg = static_cast<float>(Number(C, TEXT("banking_deg")));
			Corner.Gotchas = Lines(C, TEXT("gotchas"));
			Corner.Notes = Lines(C, TEXT("notes"));
			Corner.Cameras = Cameras(C);
		}
	}
	// The order the lap runs in, whatever order the file lists them in.
	Out.Corners.StableSort([](const FApexGuideCorner& A, const FApexGuideCorner& B) { return A.FromS < B.FromS; });
	if (Out.Track.Corners <= 0)
	{
		// Turns, not stops: the highest turn number any stop covers.
		for (const FApexGuideCorner& Corner : Out.Corners)
		{
			Out.Track.Corners = FMath::Max(Out.Track.Corners, FMath::Max(Corner.TurnTo, Corner.Number));
		}
	}
	return true;
}

bool ApexTrackGuide::ParseFileName(const FString& FileName, FString& OutStem, FString& OutClass)
{
	const FString Name = FPaths::GetCleanFilename(FileName);
	if (!Name.EndsWith(JsonSuffix(), ESearchCase::IgnoreCase))
	{
		return false;
	}
	const FString Base = Name.LeftChop(FCString::Strlen(JsonSuffix()));
	int32 Dot = INDEX_NONE;
	if (!Base.FindLastChar(TEXT('.'), Dot) || Dot <= 0 || Dot >= Base.Len() - 1)
	{
		return false;
	}
	OutStem = Base.Left(Dot);
	OutClass = Base.Mid(Dot + 1);
	return true;
}

FString ApexTrackGuide::ChooseClass(const TArray<FString>& Available, const FString& Preferred)
{
	auto Find = [&Available](const FString& Class) -> const FString*
	{
		return Class.IsEmpty() ? nullptr
			: Available.FindByPredicate([&Class](const FString& A) { return A.Equals(Class, ESearchCase::IgnoreCase); });
	};
	if (const FString* Hit = Find(Preferred.TrimStartAndEnd()))
	{
		return *Hit;
	}
	static const TCHAR* const Order[] = { TEXT("F1"), TEXT("Hypercar"), TEXT("LMP2"), TEXT("GT3") };
	for (const TCHAR* Class : Order)
	{
		if (const FString* Hit = Find(Class))
		{
			return *Hit;
		}
	}
	TArray<FString> Rest = Available;
	Rest.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
	return Rest.Num() > 0 ? Rest[0] : FString();
}

FString ApexTrackGuide::DirectionLabel(const FString& Direction)
{
	const FString Key = Direction.TrimStartAndEnd().ToLower();
	if (Key == TEXT("left")) { return TEXT("LEFT"); }
	if (Key == TEXT("right")) { return TEXT("RIGHT"); }
	if (Key == TEXT("left-right")) { return TEXT("LEFT-RIGHT"); }
	if (Key == TEXT("right-left")) { return TEXT("RIGHT-LEFT"); }
	return Key.ToUpper();
}

FString ApexTrackGuide::TurnLabel(const FApexGuideCorner& Corner)
{
	const int32 From = Corner.TurnFrom > 0 ? Corner.TurnFrom : Corner.Number;
	const int32 To = FMath::Max(From, Corner.TurnTo);
	return To > From ? FString::Printf(TEXT("TURNS %d–%d"), From, To) : FString::Printf(TEXT("TURN %d"), From);
}

FString ApexTrackGuide::TrackDirectionLabel(const FString& Direction)
{
	const FString Key = Direction.TrimStartAndEnd().ToLower();
	if (Key == TEXT("clockwise")) { return TEXT("Clockwise"); }
	if (Key == TEXT("anticlockwise") || Key == TEXT("counterclockwise") || Key == TEXT("counter-clockwise")) { return TEXT("Anticlockwise"); }
	return FString();
}

FString ApexTrackGuide::BrakeText(const FApexGuideCorner& Corner)
{
	if (Corner.bFlatOut)
	{
		return TEXT("Flat out");
	}
	if (Corner.bHasBrake && Corner.BrakeM > 0.0f)
	{
		return FString::Printf(TEXT("Brake %.0f m before the apex"), Corner.BrakeM);
	}
	return FString();
}

FString ApexTrackGuide::LapTimeText(float Seconds)
{
	if (Seconds <= 0.0f)
	{
		return TEXT("—");
	}
	const int32 Ms = FMath::RoundToInt(Seconds * 1000.0f);
	return FString::Printf(TEXT("%d:%02d.%03d"), Ms / 60000, (Ms / 1000) % 60, Ms % 1000);
}

FString ApexTrackGuide::SpeedText(float Kph, bool bMetric)
{
	if (Kph <= 0.0f)
	{
		return TEXT("—");
	}
	return bMetric ? FString::Printf(TEXT("%.0f km/h"), Kph) : FString::Printf(TEXT("%.0f mph"), Kph * 0.621371f);
}

FString ApexTrackGuide::DistanceText(float Metres, bool bMetric)
{
	if (Metres <= 0.0f)
	{
		return TEXT("—");
	}
	if (bMetric)
	{
		return Metres >= 1000.0f ? FString::Printf(TEXT("%.3f km"), Metres / 1000.0f) : FString::Printf(TEXT("%.0f m"), Metres);
	}
	const float Miles = Metres / 1609.344f;
	return Miles >= 0.5f ? FString::Printf(TEXT("%.2f mi"), Miles) : FString::Printf(TEXT("%.0f ft"), Metres * 3.28084f);
}

FString ApexTrackGuide::HeightText(float Metres, bool bMetric, bool bSigned)
{
	const float Value = bMetric ? Metres : Metres * 3.28084f;
	const TCHAR* Unit = bMetric ? TEXT("m") : TEXT("ft");
	const int32 Rounded = FMath::RoundToInt(Value);
	if (bSigned && Rounded != 0)
	{
		return FString::Printf(TEXT("%s%d %s"), Rounded > 0 ? TEXT("+") : TEXT("-"), FMath::Abs(Rounded), Unit);
	}
	return FString::Printf(TEXT("%d %s"), Rounded, Unit);
}

TArray<FString> ApexTrackGuide::CornerLines(const FApexGuideCorner& Corner)
{
	// The hand-written notes first: somebody chose to say them.
	TArray<FString> Lines = Corner.Notes;
	for (const FString& Line : Corner.Gotchas)
	{
		Lines.AddUnique(Line);
	}
	return Lines;
}
