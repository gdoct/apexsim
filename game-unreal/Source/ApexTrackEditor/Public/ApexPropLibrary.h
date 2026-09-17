#pragma once

#include "CoreMinimal.h"

/**
 * What the importer knows about the authored prop kit (`content/props`,
 * see docs/PROPS.md): which kinds are instanced, which get Nanite, which
 * face the road, what each kind falls back to, the aliases that keep the
 * pre-kit `.ats` scenes working, where the imported assets live, and the
 * pure maths of laying a grandstand out of bays.
 *
 * Data and arithmetic only — nothing here touches assets — so it is shared
 * by `ApexPropImport` (which brings the GLBs in) and the track asset
 * builder (which places them) and is covered by `ApexSim.Props.*` tests.
 */
namespace ApexProps
{
	/** Content root the kit is imported under. */
	inline const TCHAR* const DefaultRoot = TEXT("/Game/Props");

	/** Per-kind import and placement behaviour. */
	struct FKindInfo
	{
		const TCHAR* Kind;
		/** Placed through one instanced component per (kind, asset) instead of an actor each. */
		bool bInstanced;
		/** Built with Nanite on import: the big one-offs. */
		bool bNanite;
		/** Turned so its local +Y (the authored front) faces the nearest road. */
		bool bFaceRoad;
		/** Asset used when the prop's own is not authored; empty for none. */
		const TCHAR* DefaultAsset;
	};

	APEXTRACKEDITOR_API const FKindInfo* FindKind(const FString& Kind);
	APEXTRACKEDITOR_API bool IsInstancedKind(const FString& Kind);
	APEXTRACKEDITOR_API bool IsNaniteKind(const FString& Kind);
	/**
	 * Whether a resolved prop is turned to face the road. Per kind, with the
	 * odd asset that differs: a video screen is an `attraction` but has a
	 * front, a ferris wheel does not.
	 */
	APEXTRACKEDITOR_API bool FacesRoad(const FString& Kind, const FString& Asset);
	/**
	 * Whether a resolved prop is turned to look back up the course instead
	 * of across it: a braking marker and a marshal light panel are read by
	 * a driver on the way in, so their authored front (+Y, like every other
	 * board) has to point against the direction of travel rather than at the
	 * road. The builder yaws them +90 degrees and never flips them by side.
	 */
	APEXTRACKEDITOR_API bool FacesUpCourse(const FString& Kind, const FString& Asset);
	/** Degrees added to a prop's yaw so its front looks back up the course. */
	inline constexpr float UpCourseYawDeg = 90.0f;
	/** Empty when the kind has no default. */
	APEXTRACKEDITOR_API FString DefaultAssetFor(const FString& Kind);
	/** Every kind the kit knows, for the importer's folder walk. */
	APEXTRACKEDITOR_API TArray<FString> AllKinds();

	/**
	 * Map the asset keys the pre-kit scenes carry onto authored assets, in
	 * place. May change the kind (`sign/board_200m` is a `board`), and fills
	 * in `Text` when the alias implies one and the prop has none. Returns
	 * true when anything changed.
	 */
	APEXTRACKEDITOR_API bool ResolveAlias(FString& Kind, FString& Asset, FString& Text);

	/** `/Game/Props/<kind>/SM_<asset>` (package) and `.SM_<asset>` (object) paths. */
	APEXTRACKEDITOR_API FString MeshPackageName(const FString& Root, const FString& Kind, const FString& Asset);
	APEXTRACKEDITOR_API FString MeshObjectPath(const FString& Root, const FString& Kind, const FString& Asset);
	/** `/Game/Props/<kind>/Materials`, where a kind's materials and textures go. */
	APEXTRACKEDITOR_API FString MaterialsFolder(const FString& Root, const FString& Kind);
	/** Brand and marker textures, `T_brand_<name>` and `T_marker_<n>` (object paths). */
	APEXTRACKEDITOR_API FString BrandTextureObjectPath(const FString& Root, const FString& Brand);
	APEXTRACKEDITOR_API FString MarkerTextureObjectPath(const FString& Root, const FString& Marker);
	APEXTRACKEDITOR_API FString BrandsFolder(const FString& Root);
	APEXTRACKEDITOR_API FString MarkersFolder(const FString& Root);

	/** Material slots whose surface is the brand a prop's `text` names. */
	APEXTRACKEDITOR_API bool IsBrandSlot(FName SlotName);
	/** The braking marker's number panel; `text` is 50/100/150/200. */
	APEXTRACKEDITOR_API bool IsMarkerSlot(FName SlotName);
	/** The flag pole's cloth; `text` is a country code (`nl`, `de`, … `chequer`). */
	APEXTRACKEDITOR_API bool IsFlagSlot(FName SlotName);
	/** Flag textures, `T_flag_<code>` (object path) and their folder. */
	APEXTRACKEDITOR_API FString FlagTextureObjectPath(const FString& Root, const FString& Code);
	APEXTRACKEDITOR_API FString FlagsFolder(const FString& Root);
	/** Lamps and screens the runtime may light up. */
	APEXTRACKEDITOR_API bool IsEmissiveSlot(FName SlotName);
	/** Masked foliage and fence mesh: must import Masked and two-sided. */
	APEXTRACKEDITOR_API bool IsMaskedSlot(FName SlotName);

	// ---- Variants the scene's dressing picks --------------------------------

	/**
	 * `<asset>_crowd` for the stand assets that come with a seated crowd
	 * (every `bay_10m*`, `scaffold_10m`, `banking_seats`); empty otherwise.
	 */
	APEXTRACKEDITOR_API FString CrowdVariant(const FString& Kind, const FString& Asset);
	/** `<asset>_autumn` for the trees with autumn foliage; empty for the rest. */
	APEXTRACKEDITOR_API FString AutumnVariant(const FString& Kind, const FString& Asset);
	/**
	 * Whether a grandstand asset is a bay family (`bay_10m`, `_roof`,
	 * `_large`…): laid with end caps and turned into wedges round a bend.
	 * The other stands (`scaffold_10m`, `banking_seats`) tile plain.
	 */
	APEXTRACKEDITOR_API bool IsBayFamily(const FString& Asset);

	/** The one asset imported as two meshes: the wheel and its `rotor` node. */
	inline const TCHAR* const FerrisWheelKind = TEXT("attraction");
	inline const TCHAR* const FerrisWheelAsset = TEXT("ferris_wheel");
	inline const TCHAR* const FerrisRotorAsset = TEXT("ferris_wheel_rotor");
	/** Where the rotor node sits in the wheel's frame: the hub, 35 m up. */
	inline const FVector FerrisHubOffsetCm(0.0, 0.0, 3500.0);

	/** The authored start gantry's lamp panel, in the gantry's frame (cm). */
	inline constexpr float GantryLampX = -37.0f;
	inline constexpr float GantryLampPitchY = 80.0f;
	inline constexpr float GantryLampZ = 545.0f;
	inline constexpr float GantryLampRadius = 22.0f;
	/** The road width a bridge is authored for; local Y scales by span / this. */
	inline constexpr float BridgeAuthoredSpanM = 15.0f;
	APEXTRACKEDITOR_API float BridgeSpanScale(float SpanM);

	// ---- Grandstands ---------------------------------------------------------

	/** A stand laid out of bays, in the stand's local frame (cm, UE axes). */
	struct FStandLayout
	{
		FString BayAsset;
		FString CapAsset;
		TArray<FTransform> Bays;
		TArray<FTransform> Caps;
	};

	/** Bay pitch along the stand and the wedge families' front radii. */
	inline constexpr float BayPitchM = 10.0f;
	inline constexpr float Curve6RadiusM = 95.0f;
	inline constexpr float Curve12RadiusM = 48.0f;
	/** A bend gentler than this gets straight bays. */
	inline constexpr float StraightRadiusM = 150.0f;

	/**
	 * Lay `LengthM` metres of stand from the family `Asset` (`bay_10m`,
	 * `bay_10m_roof`, `bay_10m_large`, `bay_10m_large_roof`): `round(L / 10)`
	 * bays, at least one, plus an end cap each side. With a bend radius
	 * (positive outside the corner) the wedge whose front radius is nearest
	 * is used — `_curve6` at 95 m, `_curve12` at 48 m, `_curve6_in` on the
	 * inside — and the bays are turned about the wedges' common centre so
	 * they share their side edges; the large family is straight only.
	 * `bWedge` reports whether a wedge was chosen. See docs/PROPS.md.
	 * A non-bay stand (`scaffold_10m`, `banking_seats`) is the module
	 * repeated at the same pitch, straight, with no caps.
	 */
	APEXTRACKEDITOR_API FStandLayout LayoutGrandstand(
		const FString& Asset, float LengthM, TOptional<float> RadiusM, bool* bWedge = nullptr);
}	 // namespace ApexProps
