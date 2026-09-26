#include "Track/ApexPropLibrary.h"

namespace ApexProps
{
	namespace
	{
		/*
		 * The kit's kinds (docs/PROPS.md). Instanced kinds are the ones placed
		 * by the hundred; Nanite goes on the big one-offs. Everything with a
		 * front faces the road (the buildings all have one: glass front,
		 * balcony, brand board); bridges span it, trees, parked vehicles and
		 * the sky do not care. Every kind but `cone` has an authored default.
		 */
		const FKindInfo kKinds[] = {
			//  kind            instanced  nanite  face-road  default
			{TEXT("barrier"), true, false, true, TEXT("armco_4m")},
			{TEXT("tire_wall"), true, false, true, TEXT("tires_4m")},
			{TEXT("board"), true, false, true, TEXT("hoarding_3m")},
			{TEXT("sign"), true, false, true, TEXT("marshal_post")},
			{TEXT("fence"), true, false, true, TEXT("mesh_4m")},
			{TEXT("grandstand"), false, true, true, TEXT("bay_10m")},
			{TEXT("building"), false, true, true, TEXT("clubhouse")},
			{TEXT("pit"), false, true, true, TEXT("garage_6m")},
			{TEXT("bridge"), false, true, false, TEXT("start_gantry")},
			{TEXT("light"), true, false, true, TEXT("floodlight_tower")},
			{TEXT("tree"), true, false, false, TEXT("broadleaf_m")},
			{TEXT("vehicle"), true, false, false, TEXT("car_a")},
			{TEXT("attraction"), false, true, false, TEXT("tent_6m")},
			{TEXT("sky"), false, false, false, TEXT("blimp")},
			{TEXT("cone"), true, false, false, TEXT("")},
			{TEXT("misc"), true, false, false, TEXT("bollard")},
		};

		struct FAlias
		{
			const TCHAR* Kind;
			const TCHAR* Asset;
			const TCHAR* ToKind;
			const TCHAR* ToAsset;
			/** Text implied by the old key, used when the prop carries none. */
			const TCHAR* Text;
		};

		/*
		 * What the groomer and the pre-kit scenes place, mapped onto the kit.
		 * Data rather than a migration so every shipped `.ats` keeps working
		 * untouched; the generic keys fall through to the kind defaults
		 * anyway, and are listed so the mapping is explicit in one place.
		 */
		const FAlias kAliases[] = {
			{TEXT("tree"), TEXT("tree_generic"), TEXT("tree"), TEXT("broadleaf_m"), TEXT("")},
			{TEXT("barrier"), TEXT("armco_generic"), TEXT("barrier"), TEXT("armco_4m"), TEXT("")},
			{TEXT("tire_wall"), TEXT("tire_wall_generic"), TEXT("tire_wall"), TEXT("tires_4m"), TEXT("")},
			{TEXT("sign"), TEXT("board_200m"), TEXT("board"), TEXT("braking_marker"), TEXT("200")},
			{TEXT("sign"), TEXT("board_100m"), TEXT("board"), TEXT("braking_marker"), TEXT("100")},
			{TEXT("sign"), TEXT("board_50m"), TEXT("board"), TEXT("braking_marker"), TEXT("50")},
			{TEXT("grandstand"), TEXT("grandstand_main"), TEXT("grandstand"), TEXT("bay_10m_large_roof"), TEXT("")},
			{TEXT("grandstand"), TEXT("grandstand_corner"), TEXT("grandstand"), TEXT("bay_10m_roof"), TEXT("")},
			// Only reaches the export when the scene has no pit lane to
			// generate the complex from.
			{TEXT("building"), TEXT("pit_garage"), TEXT("pit"), TEXT("garage_6m"), TEXT("")},
		};

		bool NameIs(FName SlotName, const TCHAR* Expected)
		{
			return SlotName == FName(Expected);
		}
	}	 // namespace

	const FKindInfo* FindKind(const FString& Kind)
	{
		for (const FKindInfo& Info : kKinds)
		{
			if (Kind == Info.Kind)
			{
				return &Info;
			}
		}
		return nullptr;
	}

	bool IsInstancedKind(const FString& Kind)
	{
		const FKindInfo* Info = FindKind(Kind);
		return Info && Info->bInstanced;
	}

	bool IsNaniteKind(const FString& Kind)
	{
		const FKindInfo* Info = FindKind(Kind);
		return Info && Info->bNanite;
	}

	bool FacesUpCourse(const FString& Kind, const FString& Asset)
	{
		// A distance board and a marshal light panel are read by a driver
		// coming down the road, not by the crowd across it; so are the
		// Nordschleife's German signs (chevrons, kilometre boards, the
		// bend, danger and overtaking signs).
		return Kind == TEXT("board")
			&& (Asset == TEXT("braking_marker") || Asset == TEXT("light_panel")
				|| Asset.StartsWith(TEXT("chevron_")) || Asset == TEXT("km_marker")
				|| Asset.StartsWith(TEXT("de_")));
	}

	bool FacesRoad(const FString& Kind, const FString& Asset)
	{
		if (FacesUpCourse(Kind, Asset))
		{
			// Turned up the course instead; flipping it by side as well
			// would point it at the run-off.
			return false;
		}
		if (Kind == TEXT("attraction"))
		{
			// A screen, a stage and a tent's open front look at the road; a
			// ferris wheel, a camera tower and a row of portaloos have no front.
			return Asset == TEXT("video_screen") || Asset == TEXT("fanzone_stage") || Asset == TEXT("tent_6m");
		}
		const FKindInfo* Info = FindKind(Kind);
		return Info && Info->bFaceRoad;
	}

	FString DefaultAssetFor(const FString& Kind)
	{
		const FKindInfo* Info = FindKind(Kind);
		return Info ? FString(Info->DefaultAsset) : FString();
	}

	TArray<FString> AllKinds()
	{
		TArray<FString> Kinds;
		for (const FKindInfo& Info : kKinds)
		{
			Kinds.Add(Info.Kind);
		}
		return Kinds;
	}

	bool ResolveAlias(FString& Kind, FString& Asset, FString& Text)
	{
		for (const FAlias& Alias : kAliases)
		{
			if (Kind == Alias.Kind && Asset == Alias.Asset)
			{
				Kind = Alias.ToKind;
				Asset = Alias.ToAsset;
				if (Text.IsEmpty() && Alias.Text[0] != TEXT('\0'))
				{
					Text = Alias.Text;
				}
				return true;
			}
		}
		return false;
	}

	FString MeshPackageName(const FString& Root, const FString& Kind, const FString& Asset)
	{
		return FString::Printf(TEXT("%s/%s/SM_%s"), *Root, *Kind, *Asset);
	}

	FString MeshObjectPath(const FString& Root, const FString& Kind, const FString& Asset)
	{
		return FString::Printf(TEXT("%s/%s/SM_%s.SM_%s"), *Root, *Kind, *Asset, *Asset);
	}

	FString MaterialsFolder(const FString& Root, const FString& Kind)
	{
		return FString::Printf(TEXT("%s/%s/Materials"), *Root, *Kind);
	}

	FString BrandsFolder(const FString& Root)
	{
		return Root / TEXT("board/Brands");
	}

	FString MarkersFolder(const FString& Root)
	{
		return Root / TEXT("board/Markers");
	}

	FString BrandTextureObjectPath(const FString& Root, const FString& Brand)
	{
		return FString::Printf(TEXT("%s/T_brand_%s.T_brand_%s"), *BrandsFolder(Root), *Brand, *Brand);
	}

	FString MarkerTextureObjectPath(const FString& Root, const FString& Marker)
	{
		return FString::Printf(TEXT("%s/T_marker_%s.T_marker_%s"), *MarkersFolder(Root), *Marker, *Marker);
	}

	FString FlagsFolder(const FString& Root)
	{
		return Root / TEXT("sign/Flags");
	}

	FString FlagTextureObjectPath(const FString& Root, const FString& Code)
	{
		return FString::Printf(TEXT("%s/T_flag_%s.T_flag_%s"), *FlagsFolder(Root), *Code, *Code);
	}

	const TCHAR* const DecalKind = TEXT("decal");

	TArray<FString> DecalSets()
	{
		return {TEXT("graffiti")};
	}

	FString DecalFolder(const FString& Root, const FString& Set)
	{
		// `Graffiti`, the way the other loose sets are named (`Brands`, `Flags`).
		FString Folder = Set;
		if (!Folder.IsEmpty())
		{
			Folder[0] = FChar::ToUpper(Folder[0]);
		}
		return Root / DecalKind / Folder;
	}

	FString DecalTextureObjectPath(const FString& Root, const FString& Set, const FString& Name)
	{
		const FString Asset = FString::Printf(TEXT("T_%s_%s"), *Set, *Name);
		return FString::Printf(TEXT("%s/%s.%s"), *DecalFolder(Root, Set), *Asset, *Asset);
	}

	bool ParseDecalKey(const FString& Key, FString& OutSet, FString& OutName)
	{
		static const FString Prefix = TEXT("decal_");
		if (!Key.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FString Rest = Key.RightChop(Prefix.Len());
		return Rest.Split(TEXT("_"), &OutSet, &OutName, ESearchCase::CaseSensitive, ESearchDir::FromStart)
			&& !OutSet.IsEmpty() && !OutName.IsEmpty();
	}

	bool IsFlagSlot(FName SlotName)
	{
		return NameIs(SlotName, TEXT("flag_cloth"));
	}

	FString CrowdVariant(const FString& Kind, const FString& Asset)
	{
		if (Kind != TEXT("grandstand") || Asset.EndsWith(TEXT("_crowd")))
		{
			return FString();
		}
		if (Asset.StartsWith(TEXT("bay_10m")) || Asset == TEXT("scaffold_10m") || Asset == TEXT("banking_seats"))
		{
			return Asset + TEXT("_crowd");
		}
		return FString();
	}

	FString AutumnVariant(const FString& Kind, const FString& Asset)
	{
		if (Kind != TEXT("tree") || Asset.EndsWith(TEXT("_autumn")))
		{
			return FString();
		}
		if (Asset.StartsWith(TEXT("broadleaf_")) || Asset == TEXT("poplar") || Asset == TEXT("bush_cluster"))
		{
			return Asset + TEXT("_autumn");
		}
		return FString();
	}

	bool IsBayFamily(const FString& Asset)
	{
		return Asset.StartsWith(TEXT("bay_10m"));
	}

	bool IsBrandSlot(FName SlotName)
	{
		const FString Name = SlotName.ToString();
		return Name == TEXT("board_brand") || Name == TEXT("bridge_brand") || Name.StartsWith(TEXT("bridge_brand_"))
			|| Name == TEXT("pit_team_board") || Name == TEXT("tyre_bridge_brand") || Name == TEXT("blimp_brand")
			|| Name == TEXT("balloon_envelope");
	}

	bool HasTextFace(const FString& Kind, const FString& Asset)
	{
		return Kind == TEXT("board") && Asset == TEXT("corner_sign");
	}

	bool IsMarkerSlot(FName SlotName)
	{
		return NameIs(SlotName, TEXT("board_marker"));
	}

	bool IsEmissiveSlot(FName SlotName)
	{
		return NameIs(SlotName, TEXT("gantry_lamp")) || NameIs(SlotName, TEXT("led_panel"))
			|| NameIs(SlotName, TEXT("floodlight_lamp")) || NameIs(SlotName, TEXT("led_screen"))
			|| NameIs(SlotName, TEXT("pit_light_red")) || NameIs(SlotName, TEXT("pit_light_green"));
	}

	bool IsMaskedSlot(FName SlotName)
	{
		const FString Name = SlotName.ToString();
		return Name == TEXT("fence_mesh") || Name.StartsWith(TEXT("tree_foliage")) || Name == TEXT("crowd_cards")
			|| Name.StartsWith(TEXT("tree_card_")) || Name.StartsWith(TEXT("scatter_"));
	}

	float BridgeSpanScale(float SpanM)
	{
		return SpanM > KINDA_SMALL_NUMBER ? SpanM / BridgeAuthoredSpanM : 1.0f;
	}

	FStandLayout LayoutGrandstand(const FString& Asset, float LengthM, TOptional<float> RadiusM, bool* bWedge)
	{
		FStandLayout Layout;
		const int32 Bays = FMath::Max(1, FMath::RoundToInt(LengthM / BayPitchM));
		if (!IsBayFamily(Asset))
		{
			// A club stand or a grass bank: the module repeated, no caps.
			if (bWedge)
			{
				*bWedge = false;
			}
			Layout.BayAsset = Asset;
			for (int32 i = 0; i < Bays; ++i)
			{
				const float X = (i - (Bays - 1) * 0.5f) * BayPitchM * 100.0f;
				Layout.Bays.Add(FTransform(FVector(X, 0.0, 0.0)));
			}
			return Layout;
		}
		const bool bRoof = Asset.EndsWith(TEXT("_roof"));
		const bool bLarge = Asset.Contains(TEXT("_large"));

		// Wedge choice: the family whose front radius is nearest the bend's,
		// on the inside always the one inside wedge there is. The large
		// bays come straight only.
		float ThetaDeg = 0.0f;
		FString Variant;
		if (!bLarge && RadiusM.IsSet() && FMath::Abs(RadiusM.GetValue()) <= StraightRadiusM
			&& FMath::Abs(RadiusM.GetValue()) > KINDA_SMALL_NUMBER)
		{
			const float R = RadiusM.GetValue();
			if (R < 0.0f)
			{
				ThetaDeg = -6.0f;
				Variant = TEXT("_curve6_in");
			}
			else if (R >= 0.5f * (Curve6RadiusM + Curve12RadiusM))
			{
				ThetaDeg = 6.0f;
				Variant = TEXT("_curve6");
			}
			else
			{
				ThetaDeg = 12.0f;
				Variant = TEXT("_curve12");
			}
		}
		if (bWedge)
		{
			*bWedge = ThetaDeg != 0.0f;
		}

		Layout.BayAsset = FString(TEXT("bay_10m")) + (bLarge ? TEXT("_large") : TEXT("")) + Variant
			+ (bRoof ? TEXT("_roof") : TEXT(""));
		Layout.CapAsset = bLarge ? TEXT("end_cap_large") : TEXT("end_cap");

		const float HalfPitchCm = BayPitchM * 50.0f;
		const float CapOffsetCm = HalfPitchCm + 50.0f;
		if (ThetaDeg == 0.0f)
		{
			for (int32 i = 0; i < Bays; ++i)
			{
				const float X = (i - (Bays - 1) * 0.5f) * BayPitchM * 100.0f;
				Layout.Bays.Add(FTransform(FVector(X, 0.0, 0.0)));
			}
			const float CapX = Bays * HalfPitchCm + 50.0f;
			Layout.Caps.Add(FTransform(FVector(-CapX, 0.0, 0.0)));
			Layout.Caps.Add(FTransform(FVector(CapX, 0.0, 0.0)));
			return Layout;
		}

		// A wedge's side edges converge on the y axis at Rf = 5 / tan(θ/2)
		// (95.4 m for 6°, 47.6 m for 12°). Outside a corner that point is
		// on the road side — local +Y — and the stand wraps around it;
		// inside a corner it is behind the stand (the wedge narrows toward
		// the back) and the bays turn the other way so they still run along
		// +X. Bay i is bay 0 turned about that point by (i - (n-1)/2)·θ,
		// which leaves neighbours sharing a side edge exactly.
		const float HalfTheta = FMath::DegreesToRadians(FMath::Abs(ThetaDeg)) * 0.5f;
		const float RfCm = HalfPitchCm / FMath::Tan(HalfTheta);
		const FVector Centre(0.0, ThetaDeg > 0.0f ? RfCm : -RfCm, 0.0);
		for (int32 i = 0; i < Bays; ++i)
		{
			const float PhiDeg = (i - (Bays - 1) * 0.5f) * ThetaDeg;
			const FRotator Turn(0.0f, PhiDeg, 0.0f);
			const FVector Position = Centre + Turn.RotateVector(-Centre);
			Layout.Bays.Add(FTransform(Turn, Position));
		}
		for (int32 End : {0, Bays - 1})
		{
			const FTransform& Outer = Layout.Bays[End];
			const float Along = End == 0 ? -CapOffsetCm : CapOffsetCm;
			const FVector Position = Outer.GetLocation() + Outer.GetRotation().RotateVector(FVector(Along, 0.0, 0.0));
			Layout.Caps.Add(FTransform(Outer.GetRotation(), Position));
		}
		return Layout;
	}
}	 // namespace ApexProps
