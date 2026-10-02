#include "Race/ApexCarDamage.h"

namespace ApexDamage
{
	FShares FromPercent(const float DamagePct[NumZones])
	{
		FShares Shares;
		for (int32 i = 0; i < NumZones; ++i)
		{
			// NaN fails the comparison and reads as undamaged.
			Shares.Zone[i] = DamagePct[i] > 0.0f ? FMath::Min(DamagePct[i] / 100.0f, 1.0f) : 0.0f;
		}
		return Shares;
	}

	float Visual(float Share)
	{
		return Share > 0.0f ? FMath::Pow(FMath::Min(Share, 1.0f), 0.6f) : 0.0f;
	}

	int32 ZoneIndex(EApexDamageZone Zone)
	{
		switch (Zone)
		{
		case EApexDamageZone::Rear: return Rear;
		case EApexDamageZone::Left: return Left;
		case EApexDamageZone::Right: return Right;
		default: return Front;
		}
	}

	bool IsAttached(const FApexDamagePartSpec& Part, const FShares& Shares)
	{
		return Shares.Zone[ZoneIndex(Part.Zone)] * 100.0f < Part.DetachPct;
	}

	float EngineSmokeRate(const FShares& Shares)
	{
		const float Engine = Shares.Zone[ApexDamage::Engine];
		if (Engine >= 1.0f)
		{
			return 45.0f;
		}
		const float Over = (Engine - 0.35f) / 0.65f;
		return Over > 0.0f ? 6.0f + 26.0f * Over : 0.0f;
	}

	float EngineSmokeShade(const FShares& Shares)
	{
		// Pale blue-grey oil smoke going to black as the engine lets go.
		return FMath::Lerp(0.55f, 0.08f, FMath::Clamp((Shares.Zone[ApexDamage::Engine] - 0.35f) / 0.65f, 0.0f, 1.0f));
	}

	float SteamRate(const FShares& Shares, float WaterTempC)
	{
		if (Shares.Zone[Front] < 0.12f || WaterTempC < 104.0f)
		{
			return 0.0f;
		}
		return 8.0f + 22.0f * FMath::Clamp((WaterTempC - 104.0f) / 16.0f, 0.0f, 1.0f);
	}

	float ScrapeSparkRate(float SpeedMps)
	{
		return SpeedMps > 3.0f ? FMath::Min(40.0f + 4.0f * SpeedMps, 220.0f) : 0.0f;
	}

	void StepDebris(FDebris& Debris, float DeltaSeconds)
	{
		if (Debris.bResting || DeltaSeconds <= 0.0f)
		{
			return;
		}
		const float Dt = FMath::Min(DeltaSeconds, 0.05f);
		Debris.Velocity.Z -= GravityCmPerS2 * Dt;
		// Bodywork is light and flat: the air takes a share of it each second.
		Debris.Velocity *= FMath::Exp(-0.35f * Dt);
		Debris.Location += Debris.Velocity * Dt;
		const float SpinRate = Debris.Spin.Size();
		if (SpinRate > UE_KINDA_SMALL_NUMBER)
		{
			Debris.Rotation = (FQuat(Debris.Spin / SpinRate, SpinRate * Dt) * Debris.Rotation).GetNormalized();
		}
		if (Debris.Location.Z <= Debris.GroundZ && Debris.Velocity.Z < 0.0f)
		{
			Debris.Location.Z = Debris.GroundZ;
			// A third of the fall back, the slide and the spin scrubbed by the hit.
			Debris.Velocity.Z = -Debris.Velocity.Z * 0.32f;
			Debris.Velocity.X *= 0.62f;
			Debris.Velocity.Y *= 0.62f;
			Debris.Spin *= 0.55f;
			if (Debris.Velocity.Z < 60.0f)
			{
				Debris.Velocity.Z = 0.0f;
			}
		}
		if (Debris.Location.Z <= Debris.GroundZ + 0.5f && Debris.Velocity.Z <= 0.0f)
		{
			// Sliding on the ground: friction, until it stops.
			Debris.Location.Z = Debris.GroundZ;
			Debris.Velocity.Z = 0.0f;
			const float Slide = FVector2D(Debris.Velocity.X, Debris.Velocity.Y).Size();
			const float Lost = 650.0f * Dt;
			const float Kept = Slide > Lost ? (Slide - Lost) / Slide : 0.0f;
			Debris.Velocity.X *= Kept;
			Debris.Velocity.Y *= Kept;
			Debris.Spin *= FMath::Exp(-4.0f * Dt);
			if (Kept == 0.0f && Debris.Spin.Size() < 0.3f)
			{
				Debris.bResting = true;
				Debris.Velocity = FVector::ZeroVector;
				Debris.Spin = FVector::ZeroVector;
			}
		}
	}

	bool StepPuff(FPuff& Puff, float DeltaSeconds)
	{
		if (!Puff.IsAlive())
		{
			return false;
		}
		const float Dt = FMath::Max(DeltaSeconds, 0.0f);
		Puff.Age += Dt;
		Puff.Velocity *= FMath::Exp(-Puff.Drag * Dt);
		Puff.Velocity.Z -= GravityCmPerS2 * Puff.Gravity * Dt;
		Puff.Location += Puff.Velocity * Dt;
		return Puff.IsAlive();
	}

	float PuffRadius(const FPuff& Puff)
	{
		// Square-root swell: most of the growth early, as a puff out of a
		// pipe spreads.
		return FMath::Lerp(Puff.StartSize, Puff.EndSize, FMath::Sqrt(Puff.T()));
	}

	float PuffOpacity(const FPuff& Puff)
	{
		const float T = Puff.T();
		const float In = FMath::Clamp(T / 0.1f, 0.0f, 1.0f);
		const float Out = 1.0f - FMath::Clamp((T - 0.1f) / 0.9f, 0.0f, 1.0f);
		return Puff.Opacity * In * Out * Out;
	}

	float PuffGlow(const FPuff& Puff)
	{
		const float Left = 1.0f - Puff.T();
		return Puff.Glow * Left * Left;
	}
}
