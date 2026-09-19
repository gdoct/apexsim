#pragma once

#include "CoreMinimal.h"

/**
 * What the track builder knows about the baked ground textures: which set
 * each material family samples, how big a repeat is in world metres, and
 * the maths of the seam fringe the surface bands carry in their vertex
 * colours.
 *
 * The maps themselves come from `scripts/bake_ground_textures.py` (the
 * generators live beside the prop kit's, in `content/props/_tools/
 * apex_tex.py`) and are brought in by `-run=ApexGroundTexImport`. Until
 * that has been run the assets are simply absent and the builder falls back
 * to the procedural grain it used before, so nothing here is required for a
 * level to bake.
 *
 * Data and arithmetic only — nothing here touches assets — so it is shared
 * by the importer and the asset builder and is covered by
 * `ApexSim.Track.Ground.*` tests.
 */
namespace ApexGround
{
	/** Content root the baked ground set is imported under. */
	inline const TCHAR* const TexturesRoot = TEXT("/Game/Ground");

	/** Asset name prefix, so `asphalt` + `col` is `T_ground_asphalt_col`. */
	inline const TCHAR* const TexturePrefix = TEXT("T_ground_");

	/**
	 * World size of one repeat of the baked maps, in metres. Mirrors
	 * `apex_tex.GROUND_TILE_M`; the two have to agree or every surface on
	 * every circuit is tiled at the wrong size. A look may stretch it (see
	 * `FSurfaceLook::TileM`) but this is the size the grain was authored at.
	 */
	inline constexpr float TextureTileM = 2.0f;

	/**
	 * How far the seam fringe reaches in from a surface band's edge, metres.
	 *
	 * A band is a separate mesh at its own height beside the road, so grass
	 * meets asphalt as a geometric line. Nothing in the material knows where
	 * that line is, but the exporter's `v` coordinate is metres across the
	 * band, so the builder can work out how far each vertex sits from the
	 * road-facing edge and hand the material a ramp in the vertex colour. The
	 * material then dirties the last couple of metres of the band and breaks
	 * the boundary with its macro noise. It is a fringe, not a real blend:
	 * the two surfaces still meet at a line, that line is just no longer a
	 * step between two flat colours.
	 */
	inline constexpr float EdgeSpanM = 2.5f;

	/**
	 * At most this share of a band's width becomes fringe. Astroturf strips
	 * are a metre wide, and without the cap the whole strip would be edge
	 * and none of it the surface it is meant to be.
	 */
	inline constexpr float EdgeMaxFraction = 0.35f;

	/** How a material family is dressed once the ground set is available. */
	struct FSurfaceLook
	{
		/** Baked set name (`asphalt`, `grass`, ...); empty for none. */
		const TCHAR* Set;
		/** World metres per repeat; `TextureTileM` shows the grain as authored. */
		float TileM;
		/** Multiplier on the normal map's tangent-space slope. */
		float NormalStrength;
		/** How far the roughness map may swing the family's roughness. */
		float RoughnessMapAmount;
		/** Strength of the seam fringe; 0 for anything that is not a band. */
		float EdgeBlend;
	};

	/**
	 * The look for one exported material, from its family and key. The key
	 * matters only for the `surface` family, where the exporter writes
	 * `surface_<SurfaceKind::label>` and the terrain itself is plain
	 * `ground`.
	 */
	APEXTRACKEDITOR_API FSurfaceLook LookFor(const FString& Family, const FString& Key);

	/** Every set the baker produces, for the importer and the log. */
	APEXTRACKEDITOR_API TArray<FString> AllSets();

	/** `/Game/Ground/T_ground_asphalt_col` and friends. */
	APEXTRACKEDITOR_API FString TexturePath(const FString& Set, const TCHAR* Map);

	/**
	 * Whether a material key belongs to a lateral band with two edges. The
	 * terrain (`ground`) is a grid whose UVs are world metres, not a strip,
	 * so the fringe maths would read garbage off it.
	 */
	APEXTRACKEDITOR_API bool IsSurfaceBand(const FString& Key);

	/**
	 * The fringe ramp for one vertex `FromInnerM` metres out from the
	 * road-facing edge of a band `WidthM` wide: 0 at that edge, 1 once it
	 * is `EdgeSpanM` (or `EdgeMaxFraction` of the width, whichever is less)
	 * in. One-sided on purpose — the outer edge meets the terrain, which is
	 * grass like the band, and fringing it too drew a ring of dust sixty
	 * metres out round the whole circuit.
	 */
	APEXTRACKEDITOR_API float EdgeFactor(float FromInnerM, float WidthM);

	/** One centerline sample, in the same planar frame as the band mesh. */
	struct FCenterSample
	{
		float StationM = 0.0f;
		FVector2f Location = FVector2f::ZeroVector;
		/** Unit vector across the road; which way it points does not matter. */
		FVector2f Across = FVector2f::ZeroVector;
	};

	/**
	 * The ramp for a whole band mesh, one value per vertex, all 1 (no
	 * fringe) when the centerline is missing.
	 *
	 * Neither the band's width nor which of its edges faces the road is in
	 * the mesh, and two traps make the obvious readings wrong:
	 *
	 *  - the exporter merges every chunk of one material in a section into
	 *    one mesh, so the grass on both sides of the road at one station
	 *    shares a mesh and a `u`;
	 *  - it orders each profile right to left before measuring `v`, so `v`
	 *    starts at the inner edge of a left-hand band and at the *outer*
	 *    edge of a right-hand one.
	 *
	 * So each vertex is placed against the centerline at its own station:
	 * which side it is on splits the two bands, and within one band at one
	 * station the end of the `v` range nearer the centerline is the inner
	 * edge. Stations are bucketed at millimetre resolution — the same float
	 * within a cross-section, but two sections a millimetre apart must not
	 * share a bucket. `LapM` wraps the unwrapped stations a span through
	 * start/finish carries; 0 for an open track.
	 */
	APEXTRACKEDITOR_API TArray<float> EdgeFactors(TArrayView<const FVector2f> UVs,
		TArrayView<const FVector3f> Positions, TArrayView<const FCenterSample> Centerline,
		float LapM);
}	 // namespace ApexGround
