#pragma once

#include "CoreMinimal.h"
#include "Catalog/ApexCatalogRows.h"

/**
 * What a car's damage looks like: the pure half of it (docs/content/cars.md,
 * Damage). The server sends five percentages per car (`DamagePct`: front,
 * rear, left, right, engine) and nothing about where a hit landed, so
 * everything here is worked out from those five numbers.
 *
 *  - **Dents and scuffs** are drawn by the car parent materials
 *    (`ApexCarMaterials`): the body's custom primitive data carries each
 *    zone's `Visual` amount and the body's box, and the material's world
 *    position offset pushes the zone's panels in, crumples them and scuffs
 *    the paint down to carbon and bare metal.
 *  - **Parts** (FApexDamagePartSpec) come off when their zone passes
 *    `DetachPct` and fly as debris (`StepDebris`); a repair puts them back.
 *  - **Smoke, steam and sparks** are puffs (`StepPuff`) the effects actor
 *    draws: oil smoke from a damaged engine, steam from a holed radiator
 *    running hot, sparks from a hit and from a car scraping a wall.
 */
namespace ApexDamage
{
	/** The zones in `FApexCarTelemetry::DamagePct` order. */
	enum EZone : int32
	{
		Front = 0,
		Rear = 1,
		Left = 2,
		Right = 3,
		Engine = 4,
		NumZones = 5,
	};

	/**
	 * The custom primitive data the car parents read (ApexTrackMaterialGraphs,
	 * BuildCar): the four body zones' visual amounts, the body's centre and
	 * half extents in its mesh frame (cm; a vector parameter takes four
	 * slots), and a per-car seed so two cars do not dent alike.
	 */
	inline constexpr int32 CpdFront = 0;
	inline constexpr int32 CpdRear = 1;
	inline constexpr int32 CpdLeft = 2;
	inline constexpr int32 CpdRight = 3;
	inline constexpr int32 CpdCentre = 4;
	inline constexpr int32 CpdExtent = 8;
	inline constexpr int32 CpdSeed = 12;
	inline constexpr int32 CpdCount = 13;

	/** Each zone's damage as a share, 0..1; an unknown zone (negative) is undamaged. */
	struct FShares
	{
		float Zone[NumZones] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

		bool IsZero() const
		{
			for (const float Share : Zone)
			{
				if (Share > 0.0f)
				{
					return false;
				}
			}
			return true;
		}
	};

	APEXSIM_API FShares FromPercent(const float DamagePct[NumZones]);

	/**
	 * How much of a zone's dent the material draws, 0..1, for its share of
	 * damage: a power under one, so the first few percent (a tap is 1-4%)
	 * already show as a scuff and a shallow dent, and a write-off is not a
	 * hundred times a tap.
	 */
	APEXSIM_API float Visual(float Share);

	/** The body zone a part belongs to, as an index into `FShares::Zone`. */
	APEXSIM_API int32 ZoneIndex(EApexDamageZone Zone);

	/** Whether a part is still on the car: its zone's damage short of `DetachPct`. */
	APEXSIM_API bool IsAttached(const FApexDamagePartSpec& Part, const FShares& Shares);

	/**
	 * A zone that grew by at least this many percent since the last frame
	 * took a hit worth a shower of sparks (the wire has no hit event; the
	 * engine's slow heat damage never grows this fast).
	 */
	inline constexpr float HitPercent = 0.5f;

	/** Oil smoke from the engine bay, puffs a second: none under 35% engine damage, a pall when it is out. */
	APEXSIM_API float EngineSmokeRate(const FShares& Shares);

	/** The smoke's shade, 0 black to 1 white: grey wisps, darkening as the engine goes. */
	APEXSIM_API float EngineSmokeShade(const FShares& Shares);

	/** Steam from a holed radiator, puffs a second: a damaged nose with the coolant past 104 °C. */
	APEXSIM_API float SteamRate(const FShares& Shares, float WaterTempC);

	/** Sparks a second from a car scraping along something (`bIsColliding`), by its speed. */
	APEXSIM_API float ScrapeSparkRate(float SpeedMps);

	/** A piece of bodywork in flight: centimetres, seconds, radians. */
	struct FDebris
	{
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		/** Angular velocity, rad/s, world axes. */
		FVector Spin = FVector::ZeroVector;
		/** The ground under it; a hit there bounces it. */
		float GroundZ = 0.0f;
		/** Stopped on the ground: nothing moves it any more. */
		bool bResting = false;
	};

	inline constexpr float GravityCmPerS2 = 981.0f;

	/**
	 * One step of a part's flight: gravity and a little air drag, then the
	 * ground, which bounces it (a third of its fall back), scrubs its slide
	 * and its spin, and lays it to rest once it has nearly stopped.
	 */
	APEXSIM_API void StepDebris(FDebris& Debris, float DeltaSeconds);

	/** One puff of smoke or steam, or one spark. */
	struct FPuff
	{
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;
		float Life = 1.0f;
		/** Radius at birth and at death, cm (a spark: its thickness and length). */
		float StartSize = 10.0f;
		float EndSize = 100.0f;
		/** 0 black, 1 white. */
		float Shade = 0.5f;
		/** Peak opacity. */
		float Opacity = 0.5f;
		/** Emissive multiplier at birth (sparks), fading over the life. */
		float Glow = 0.0f;
		/** Velocity lost per second, as a rate (1/s). */
		float Drag = 2.0f;
		/** Downward acceleration as a share of gravity: sparks fall, smoke rises (negative). */
		float Gravity = 0.0f;
		bool bSpark = false;

		bool IsAlive() const { return Age < Life; }
		float T() const { return Life > 0.0f ? FMath::Clamp(Age / Life, 0.0f, 1.0f) : 1.0f; }
	};

	/** Moves a puff on by `DeltaSeconds`; false once it has burnt out. */
	APEXSIM_API bool StepPuff(FPuff& Puff, float DeltaSeconds);

	/** The puff's radius now, cm: it swells quickly and then slowly. */
	APEXSIM_API float PuffRadius(const FPuff& Puff);

	/** The puff's opacity now: in over the first tenth of its life, out over the rest. */
	APEXSIM_API float PuffOpacity(const FPuff& Puff);

	/** A spark's glow now: white-hot at birth, out by the end. */
	APEXSIM_API float PuffGlow(const FPuff& Puff);
}
