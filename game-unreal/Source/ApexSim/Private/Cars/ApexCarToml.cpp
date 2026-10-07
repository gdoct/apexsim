#include "Cars/ApexCarToml.h"

namespace
{
	/** A TOML value with its quotes and any trailing comment removed. */
	FString TomlValue(const FString& Raw)
	{
		FString Value = Raw;
		int32 Hash = INDEX_NONE;
		// A '#' outside quotes starts a comment.
		bool bInString = false;
		for (int32 i = 0; i < Value.Len(); ++i)
		{
			if (Value[i] == TEXT('"'))
			{
				bInString = !bInString;
			}
			else if (Value[i] == TEXT('#') && !bInString)
			{
				Hash = i;
				break;
			}
		}
		if (Hash != INDEX_NONE)
		{
			Value.LeftInline(Hash);
		}
		Value.TrimStartAndEndInline();
		if (Value.Len() >= 2 && Value[0] == TEXT('"') && Value[Value.Len() - 1] == TEXT('"'))
		{
			Value.MidInline(1, Value.Len() - 2);
		}
		return Value;
	}

	/** `[1, 2, ...]` -> `Count` numbers; false unless it is exactly that many. */
	bool TomlNumbers(const FString& Value, double* Out, int32 Count)
	{
		FString Inner = Value;
		Inner.TrimStartAndEndInline();
		if (!Inner.StartsWith(TEXT("[")) || !Inner.EndsWith(TEXT("]")))
		{
			return false;
		}
		Inner.MidInline(1, Inner.Len() - 2);
		TArray<FString> Parts;
		Inner.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != Count)
		{
			return false;
		}
		for (int32 i = 0; i < Count; ++i)
		{
			Out[i] = FCString::Atod(*Parts[i].TrimStartAndEnd());
		}
		return true;
	}

	/** `[1, 2, 3]` -> three numbers; false unless it is exactly that. */
	bool TomlTriple(const FString& Value, double Out[3])
	{
		return TomlNumbers(Value, Out, 3);
	}

	/** `[1, 2]` -> a pair; false unless it is exactly two numbers. */
	bool TomlPair(const FString& Value, FVector2D& Out)
	{
		double V[2];
		if (!TomlNumbers(Value, V, 2))
		{
			return false;
		}
		Out = FVector2D(V[0], V[1]);
		return true;
	}

	/** `[0.1, 0.2, 0.3]` -> an opaque linear colour; false unless it is three numbers. */
	bool TomlColour(const FString& Value, FLinearColor& Out)
	{
		double V[3];
		if (!TomlTriple(Value, V))
		{
			return false;
		}
		Out = FLinearColor(static_cast<float>(V[0]), static_cast<float>(V[1]), static_cast<float>(V[2]), 1.0f);
		return true;
	}

	bool TomlVector(const FString& Value, FVector& Out)
	{
		double V[3];
		if (!TomlTriple(Value, V))
		{
			return false;
		}
		Out = FVector(V[0], V[1], V[2]);
		return true;
	}

	/**
	 * `["a", "b"]` -> two strings; false unless it is exactly an array of
	 * quoted strings on the one line (the scan is line by line). Commas inside
	 * the quotes are the string's.
	 */
	bool TomlStringArray(const FString& Value, TArray<FString>& Out)
	{
		Out.Reset();
		FString Inner = Value;
		Inner.TrimStartAndEndInline();
		if (!Inner.StartsWith(TEXT("[")) || !Inner.EndsWith(TEXT("]")))
		{
			return false;
		}
		Inner.MidInline(1, Inner.Len() - 2);
		bool bInString = false;
		bool bNeedComma = false;
		FString Current;
		for (const TCHAR C : Inner)
		{
			if (bInString)
			{
				if (C == TEXT('"'))
				{
					bInString = false;
					bNeedComma = true;
					Out.Add(Current);
					Current.Reset();
				}
				else
				{
					Current.AppendChar(C);
				}
			}
			else if (C == TEXT('"'))
			{
				if (bNeedComma)
				{
					return false;
				}
				bInString = true;
			}
			else if (C == TEXT(','))
			{
				if (!bNeedComma)
				{
					return false;
				}
				bNeedComma = false;
			}
			else if (!FChar::IsWhitespace(C))
			{
				return false;
			}
		}
		return !bInString;
	}

	/** `true` / `false`; false (and `bOk` cleared) for anything else. */
	bool TomlBool(const FString& Value, bool& bOk)
	{
		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (!Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			bOk = false;
		}
		return false;
	}
}	 // namespace

FString ApexCarToml::Segment(const FString& Folder)
{
	FString Segment;
	for (const TCHAR C : Folder)
	{
		Segment.AppendChar(FChar::IsAlnum(C) ? C : TEXT('_'));
	}
	return Segment;
}

bool ApexCarToml::IsCarLocalWheel(const FString& Model)
{
	return Model.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase) || Model.Contains(TEXT("/")) || Model.Contains(TEXT("\\"));
}

FApexWheelSpec ApexCarToml::MakeWheelSpec(const FApexCarToml& Toml)
{
	FApexWheelSpec Spec;
	const FApexCarWheelsToml& W = Toml.Wheels;
	if (!W.IsPresent())
	{
		return Spec;
	}
	Spec.FrontAxleM = W.FrontAxleM;
	Spec.RearAxleM = W.RearAxleM;
	Spec.FrontTrackM = W.FrontTrackM;
	Spec.RearTrackM = W.RearTrackM;
	Spec.FrontRadiusM = W.FrontRadiusM;
	Spec.RearRadiusM = W.RearRadiusM;
	Spec.FrontWidthM = W.FrontWidthM;
	Spec.RearWidthM = W.RearWidthM;
	Spec.MaxSteerRad = Toml.MaxSteerRad;
	return Spec;
}

FApexDrsFlapSpec ApexCarToml::MakeDrsFlapSpec(const FApexCarToml& Toml)
{
	FApexDrsFlapSpec Spec;
	if (!Toml.DrsFlap.IsPresent())
	{
		return Spec;
	}
	Spec.HingeForwardM = Toml.DrsFlap.HingeForwardM;
	Spec.HingeUpM = Toml.DrsFlap.HingeUpM;
	Spec.OpenDeg = Toml.DrsFlap.OpenDeg;
	return Spec;
}

TArray<FApexCompoundSpec> ApexCarToml::DefaultCompounds()
{
	TArray<FApexCompoundSpec> Compounds;
	const TPair<const TCHAR*, EApexCompoundKind> Defaults[] = {
		{ TEXT("soft"), EApexCompoundKind::Slick }, { TEXT("medium"), EApexCompoundKind::Slick },
		{ TEXT("hard"), EApexCompoundKind::Slick }, { TEXT("intermediate"), EApexCompoundKind::Intermediate },
		{ TEXT("wet"), EApexCompoundKind::Wet } };
	for (const TPair<const TCHAR*, EApexCompoundKind>& Default : Defaults)
	{
		FApexCompoundSpec& Spec = Compounds.AddDefaulted_GetRef();
		Spec.Name = Default.Key;
		Spec.Kind = Default.Value;
	}
	return Compounds;
}

TArray<FApexCompoundSpec> ApexCarToml::MakeCompounds(const FApexCarToml& Toml)
{
	if (Toml.Compounds.IsEmpty())
	{
		return DefaultCompounds();
	}
	TArray<FApexCompoundSpec> Compounds;
	for (const FApexCarCompoundToml& Source : Toml.Compounds)
	{
		FApexCompoundSpec& Spec = Compounds.AddDefaulted_GetRef();
		Spec.Name = Source.Name;
		Spec.Kind = Source.Kind == TEXT("wet") ? EApexCompoundKind::Wet
			: Source.Kind == TEXT("intermediate") ? EApexCompoundKind::Intermediate
												   : EApexCompoundKind::Slick;
	}
	return Compounds;
}

TArray<FApexDamagePartSpec> ApexCarToml::MakeDamageParts(const FApexCarToml& Toml)
{
	TArray<FApexDamagePartSpec> Parts;
	for (const FApexCarDamagePartToml& Source : Toml.DamageParts)
	{
		FApexDamagePartSpec& Part = Parts.AddDefaulted_GetRef();
		Part.Name = Source.Name;
		Part.Zone = Source.Zone == TEXT("rear") ? EApexDamageZone::Rear
			: Source.Zone == TEXT("left")       ? EApexDamageZone::Left
			: Source.Zone == TEXT("right")      ? EApexDamageZone::Right
												: EApexDamageZone::Front;
		Part.DetachPct = Source.DetachPct;
		// The car's (forward, left, up) metres onto the body mesh's
		// (left, forward, up) centimetres.
		auto ToMesh = [](const FVector& M) { return FVector(M.Y, M.X, M.Z) * 100.0; };
		Part.MinCm = ToMesh(Source.MinM).ComponentMin(ToMesh(Source.MaxM));
		Part.MaxCm = ToMesh(Source.MinM).ComponentMax(ToMesh(Source.MaxM));
	}
	return Parts;
}

bool ApexCarToml::Parse(const FString& Text, FApexCarToml& Out, FString& OutError)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	FString Table;
	// A value the scan cannot take (a switch that is neither true nor false),
	// reported once the file is read.
	FString ValueError;
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		if (Line.StartsWith(TEXT("[[livery]]")))
		{
			Out.Liveries.AddDefaulted();
		}
		if (Line.StartsWith(TEXT("[[damage_part]]")))
		{
			Out.DamageParts.AddDefaulted();
		}
		if (Line.StartsWith(TEXT("[[tires.compound]]")))
		{
			Out.Compounds.AddDefaulted();
		}
		if (Line.StartsWith(TEXT("[")))
		{
			// `[physics]`, `[[engine.torque_curve]]`: everything after this
			// belongs to that table until the next header.
			Table = Line.Replace(TEXT("["), TEXT("")).Replace(TEXT("]"), TEXT("")).TrimStartAndEnd();
			if (Table == TEXT("preview"))
			{
				Out.Preview.bPresent = true;
			}
			else if (Table == TEXT("cockpit"))
			{
				Out.bHasCockpit = true;
			}
			continue;
		}
		FString Key;
		FString Raw;
		if (!Line.Split(TEXT("="), &Key, &Raw))
		{
			continue;
		}
		Key.TrimStartAndEndInline();
		const FString Value = TomlValue(Raw);
		if (Table.IsEmpty())
		{
			if (Key == TEXT("id")) { Out.Id = Value; }
			else if (Key == TEXT("name")) { Out.Name = Value; }
			else if (Key == TEXT("model")) { Out.Model = Value; }
			else if (Key == TEXT("brand")) { Out.Brand = Value; }
			else if (Key == TEXT("class")) { Out.CarClass = Value; }
			else if (Key == TEXT("manufacturer_country")) { Out.ManufacturerCountry = Value; }
			else if (Key == TEXT("model_year")) { Out.ModelYear = FCString::Atoi(*Value); }
		}
		else if (Table == TEXT("physics") && Key == TEXT("mass_kg"))
		{
			Out.MassKg = FCString::Atof(*Value);
		}
		else if (Table == TEXT("physics") && Key == TEXT("max_steering_angle_rad"))
		{
			Out.MaxSteerRad = FCString::Atof(*Value);
		}
		else if (Table == TEXT("wheels"))
		{
			FApexCarWheelsToml& W = Out.Wheels;
			const float Number = FCString::Atof(*Value);
			if (Key == TEXT("model")) { W.Model = Value; }
			else if (Key == TEXT("rear_model")) { W.RearModel = Value; }
			else if (Key == TEXT("front_axle_m")) { W.FrontAxleM = Number; }
			else if (Key == TEXT("rear_axle_m")) { W.RearAxleM = Number; }
			else if (Key == TEXT("front_track_m")) { W.FrontTrackM = Number; }
			else if (Key == TEXT("rear_track_m")) { W.RearTrackM = Number; }
			else if (Key == TEXT("front_radius_m")) { W.FrontRadiusM = Number; }
			else if (Key == TEXT("rear_radius_m")) { W.RearRadiusM = Number; }
			else if (Key == TEXT("front_width_m")) { W.FrontWidthM = Number; }
			else if (Key == TEXT("rear_width_m")) { W.RearWidthM = Number; }
		}
		else if (Table == TEXT("drs_flap"))
		{
			FApexCarDrsFlapToml& D = Out.DrsFlap;
			const float Number = FCString::Atof(*Value);
			if (Key == TEXT("model")) { D.Model = Value; }
			else if (Key == TEXT("hinge_forward_m")) { D.HingeForwardM = Number; }
			else if (Key == TEXT("hinge_up_m")) { D.HingeUpM = Number; }
			else if (Key == TEXT("open_deg")) { D.OpenDeg = Number; }
		}
		else if (Table == TEXT("driver"))
		{
			if (Key == TEXT("model")) { Out.Driver.Model = Value; }
		}
		else if (Table == TEXT("engine"))
		{
			// `[[engine.torque_curve]]` has an `rpm` of its own; it is another table.
			const float Number = FCString::Atof(*Value);
			if (Key == TEXT("max_power_w")) { Out.MaxPowerKw = Number / 1000.0f; }
			else if (Key == TEXT("idle_rpm")) { Out.Sound.IdleRpm = Number; }
			else if (Key == TEXT("redline_rpm")) { Out.Sound.RedlineRpm = Number; }
			else if (Key == TEXT("rev_limiter_rpm")) { Out.Sound.LimiterRpm = Number; }
		}
		else if (Table == TEXT("livery") && Out.Liveries.Num() > 0)
		{
			FApexCarLiveryToml& L = Out.Liveries.Last();
			if (Key == TEXT("name")) { L.Name = Value; }
			else if (Key == TEXT("paint")) { TomlColour(Value, L.Paint); }
			else if (Key == TEXT("accent")) { TomlColour(Value, L.Accent); }
			else if (Key == TEXT("metallic")) { L.Metallic = FCString::Atof(*Value); }
			else if (Key == TEXT("logo")) { L.Logo = Value; }
			else if (Key == TEXT("skin")) { L.Skin = Value; }
			else if (Key == TEXT("preview")) { L.Preview = Value; }
			else if (Key == TEXT("textures"))
			{
				// `["EXT_RIM=skins/x/EXT_RIM.png", ...]`: slot, then the file.
				TArray<FString> Entries;
				L.Textures.Reset();
				L.bBadTextures = !TomlStringArray(Value, Entries);
				for (const FString& Entry : Entries)
				{
					FString Slot;
					FString File;
					if (!Entry.Split(TEXT("="), &Slot, &File) || Slot.TrimStartAndEnd().IsEmpty() || File.TrimStartAndEnd().IsEmpty())
					{
						L.bBadTextures = true;
						continue;
					}
					L.Textures.Emplace(Slot.TrimStartAndEnd(), File.TrimStartAndEnd());
				}
			}
		}
		else if (Table == TEXT("damage_part") && Out.DamageParts.Num() > 0)
		{
			FApexCarDamagePartToml& P = Out.DamageParts.Last();
			if (Key == TEXT("name")) { P.Name = Value; }
			else if (Key == TEXT("zone")) { P.Zone = Value.ToLower(); }
			else if (Key == TEXT("detach_pct")) { P.DetachPct = FCString::Atof(*Value); }
			else if (Key == TEXT("min_m")) { P.bBadBox |= !TomlVector(Value, P.MinM); }
			else if (Key == TEXT("max_m")) { P.bBadBox |= !TomlVector(Value, P.MaxM); }
		}
		else if (Table == TEXT("tires"))
		{
			if (Key == TEXT("optimal_temperature_c")) { Out.TyreOptimalC = FCString::Atof(*Value); }
			else if (Key == TEXT("temperature_window_c")) { Out.TyreWindowC = FCString::Atof(*Value); }
		}
		else if (Table == TEXT("tires.compound") && Out.Compounds.Num() > 0)
		{
			FApexCarCompoundToml& C = Out.Compounds.Last();
			if (Key == TEXT("name")) { C.Name = Value; }
			else if (Key == TEXT("kind")) { C.Kind = Value.ToLower(); }
		}
		else if (Table == TEXT("sound"))
		{
			FApexEngineSoundSpec& S = Out.Sound;
			const float Number = FCString::Atof(*Value);
			const bool bTrue = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			if (Key == TEXT("cylinders")) { S.Cylinders = FCString::Atoi(*Value); }
			else if (Key == TEXT("crossplane")) { S.bCrossplane = bTrue; }
			else if (Key == TEXT("turbo")) { S.bTurbo = bTrue; }
			else if (Key == TEXT("exhaust_length_m")) { S.ExhaustLengthM = Number; }
			else if (Key == TEXT("muffling")) { S.Muffling = Number; }
			else if (Key == TEXT("pops")) { S.Pops = Number; }
			else if (Key == TEXT("gear_whine")) { S.GearWhine = Number; }
			else if (Key == TEXT("intake_roar")) { S.IntakeRoar = Number; }
		}
		else if (Table == TEXT("preview"))
		{
			FApexCarPreviewToml& P = Out.Preview;
			FVector Triple;
			if (Key == TEXT("offset_cm") && TomlVector(Value, Triple)) { P.OffsetCm = Triple; }
			else if (Key == TEXT("rotation_deg") && TomlVector(Value, Triple)) { P.Rotation = FRotator(Triple.X, Triple.Y, Triple.Z); }
			else if (Key == TEXT("scale")) { P.Scale = FCString::Atof(*Value); }
		}
		else if (Table == TEXT("cockpit"))
		{
			FApexCockpitOverrides& C = Out.Cockpit;
			if (Key == TEXT("style"))
			{
				C.Style = Value.Equals(TEXT("open"), ESearchCase::IgnoreCase) ? EApexCockpitStyle::OpenWheel
					: Value.Equals(TEXT("closed"), ESearchCase::IgnoreCase)   ? EApexCockpitStyle::Closed
																			  : EApexCockpitStyle::Auto;
			}
			else if (Key == TEXT("eye_cm")) { TomlVector(Value, C.Eye); }
			else if (Key == TEXT("wheel_cm")) { TomlVector(Value, C.Wheel); }
			else if (Key == TEXT("mirror_centre_cm")) { TomlVector(Value, C.MirrorCentre); }
			else if (Key == TEXT("mirror_left_cm")) { TomlVector(Value, C.MirrorLeft); }
			else if (Key == TEXT("mirror_right_cm")) { TomlVector(Value, C.MirrorRight); }
			else if (Key == TEXT("mirror_centre_size_cm")) { TomlPair(Value, C.MirrorCentreSizeCm); }
			else if (Key == TEXT("mirror_left_size_cm")) { TomlPair(Value, C.MirrorLeftSizeCm); }
			else if (Key == TEXT("mirror_right_size_cm")) { TomlPair(Value, C.MirrorRightSizeCm); }
			else if (Key == TEXT("wheel_rake_deg")) { C.WheelRakeDeg = FCString::Atof(*Value); }
			else if (Key == TEXT("wheel_lock_deg")) { C.WheelLockDeg = FCString::Atof(*Value); }
			else if (Key == TEXT("steering_wheel_model")) { Out.SteeringWheelModel = Value; }
			else if (Key == TEXT("rig_wheel") || Key == TEXT("rig_dash"))
			{
				bool bOk = true;
				const bool bOn = TomlBool(Value, bOk);
				if (!bOk)
				{
					ValueError = FString::Printf(TEXT("[cockpit] %s must be true or false"), *Key);
				}
				(Key == TEXT("rig_wheel") ? C.bRigWheel : C.bRigDash) = bOn;
			}
		}
	}
	if (Out.Id.IsEmpty() || Out.Name.IsEmpty())
	{
		OutError = TEXT("car.toml has no id or name");
		return false;
	}
	if (!ValueError.IsEmpty())
	{
		OutError = ValueError;
		return false;
	}
	const FApexCarWheelsToml& W = Out.Wheels;
	if (W.IsPresent()
		&& (W.FrontRadiusM <= 0.0f || W.RearRadiusM <= 0.0f || W.FrontWidthM <= 0.0f || W.RearWidthM <= 0.0f
			|| W.FrontTrackM <= 0.0f || W.RearTrackM <= 0.0f || W.FrontAxleM <= W.RearAxleM))
	{
		OutError = TEXT("[wheels] needs positive radii, widths and tracks, and the front axle ahead of the rear");
		return false;
	}
	const FApexCarDrsFlapToml& D = Out.DrsFlap;
	if (D.IsPresent() && (D.HingeUpM <= 0.0f || D.OpenDeg <= 0.0f || D.OpenDeg > 60.0f))
	{
		OutError = TEXT("[drs_flap] needs a hinge above the floor and an open_deg of 0-60");
		return false;
	}
	for (const FApexCarLiveryToml& L : Out.Liveries)
	{
		// A texture livery may leave the paint as the model has it.
		if (L.Name.IsEmpty() || (!L.HasPaint() && L.Skin.IsEmpty() && L.Textures.IsEmpty()))
		{
			OutError = TEXT("every [[livery]] needs a name and a paint = [r, g, b], a skin or textures");
			return false;
		}
		if (L.bBadTextures)
		{
			OutError = FString::Printf(TEXT("[[livery]] %s: textures must be one line of [\"SLOT=file\", ...]"), *L.Name);
			return false;
		}
	}
	for (const FApexCarCompoundToml& C : Out.Compounds)
	{
		if (C.Name.IsEmpty() || !(C.Kind.IsEmpty() || C.Kind == TEXT("slick") || C.Kind == TEXT("intermediate") || C.Kind == TEXT("wet")))
		{
			OutError = FString::Printf(TEXT("[[tires.compound]] %s needs a name and a kind of slick, intermediate or wet"), *C.Name);
			return false;
		}
	}
	for (const FApexCarDamagePartToml& P : Out.DamageParts)
	{
		const bool bZone = P.Zone == TEXT("front") || P.Zone == TEXT("rear") || P.Zone == TEXT("left") || P.Zone == TEXT("right");
		if (P.Name.IsEmpty() || !bZone || P.DetachPct <= 0.0f || P.DetachPct > 100.0f || P.bBadBox
			|| P.MinM.X >= P.MaxM.X || P.MinM.Y >= P.MaxM.Y || P.MinM.Z >= P.MaxM.Z)
		{
			OutError = FString::Printf(
				TEXT("[[damage_part]] %s needs a name, a zone of front, rear, left or right, a detach_pct of 0-100 and min_m below max_m on every axis"),
				*P.Name);
			return false;
		}
	}
	if (Out.DamageParts.Num() > 8)
	{
		OutError = TEXT("at most 8 [[damage_part]] tables");
		return false;
	}
	if (Out.Cockpit.WheelLockDeg < 0.0f || Out.Cockpit.WheelLockDeg > 1080.0f || FMath::Abs(Out.Cockpit.WheelRakeDeg) > 90.0f)
	{
		OutError = TEXT("[cockpit] wheel_lock_deg must be 0-1080 and wheel_rake_deg within +-90");
		return false;
	}
	if (Out.Liveries.Num() > 254)
	{
		OutError = TEXT("at most 254 [[livery]] tables: the wire carries the pick in a byte");
		return false;
	}
	const FApexEngineSoundSpec& S = Out.Sound;
	auto IsShare = [](float Value) { return Value >= 0.0f && Value <= 1.0f; };
	if (S.Cylinders != 0
		&& (S.Cylinders < 1 || S.Cylinders > 16 || S.ExhaustLengthM < 0.2f || S.ExhaustLengthM > 2.8f
			|| !IsShare(S.Muffling) || !IsShare(S.Pops) || !IsShare(S.GearWhine) || !IsShare(S.IntakeRoar)))
	{
		OutError = TEXT("[sound] needs 1-16 cylinders, an exhaust_length_m of 0.2-2.8 and muffling, pops, gear_whine and intake_roar within 0-1");
		return false;
	}
	if (Out.Preview.bPresent && Out.Preview.Scale <= 0.0f)
	{
		OutError = TEXT("[preview] scale must be positive");
		return false;
	}
	return true;
}
