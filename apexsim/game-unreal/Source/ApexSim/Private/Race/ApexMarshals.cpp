#include "Race/ApexMarshals.h"

#include "Race/ApexTvDirector.h"

namespace ApexMarshals
{
	namespace
	{
		FVector RightOfLevel(const FVector& Along)
		{
			return FVector(-Along.Y, Along.X, 0.0).GetSafeNormal();
		}

		double SmoothStep01(double T)
		{
			T = FMath::Clamp(T, 0.0, 1.0);
			return T * T * (3.0 - 2.0 * T);
		}
	}

	FVector FIncident::TruckLocation() const
	{
		const double T = SmoothStep01((Age - TruckAfterS) / TruckDriveS);
		return FMath::Lerp(TruckFrom, TruckTo, T);
	}

	float FIncident::TruckYawDeg() const
	{
		const FVector Way = (TruckTo - TruckFrom).GetSafeNormal2D();
		return static_cast<float>((Way.IsNearlyZero() ? Along : Way).Rotation().Yaw);
	}

	void FTracker::Reset()
	{
		Memory.Reset();
		Incidents.Reset();
	}

	FIncident FTracker::Open(const FCarSample& Car, const ApexTv::FPath* Path) const
	{
		FIncident Scene;
		Scene.CarIndex = Car.CarIndex;
		Scene.CarLocation = Car.Location;

		if (Path && Path->IsValid())
		{
			const double Station = Path->StationAt(Car.Location);
			FVector Point;
			FVector Tangent;
			Path->Sample(Station, Point, Tangent);
			Scene.Along = Tangent.GetSafeNormal2D();
			const FVector Right = RightOfLevel(Scene.Along);
			const double Lateral = FVector::DotProduct(FVector(Car.Location.X - Point.X, Car.Location.Y - Point.Y, 0.0), Right);
			// The car's own side of the road; a car on the centerline gets the right.
			const double Side = Lateral < 0.0 ? -1.0 : 1.0;
			Scene.Outward = Right * Side;

			// Up the road, at the verge on the car's side (or further out
			// than the car when it went off): the post the cars come up to.
			FVector UpPoint;
			FVector UpTangent;
			Path->Sample(Station - PostUpCm, UpPoint, UpTangent);
			const double PostLateral = FMath::Max(FMath::Abs(Lateral) + PostOutCm, PostMinLateralCm);
			Scene.Post = UpPoint + RightOfLevel(UpTangent.GetSafeNormal2D()) * Side * PostLateral;

			// The truck comes along the same line as the car, a little further out.
			const double TruckLateral = Lateral + Side * TruckBesideCm;
			FVector FromPoint;
			FVector FromTangent;
			Path->Sample(Station - TruckFromCm, FromPoint, FromTangent);
			Scene.TruckFrom = FromPoint + RightOfLevel(FromTangent.GetSafeNormal2D()) * TruckLateral;
			FVector ToPoint;
			FVector ToTangent;
			Path->Sample(Station - TruckStopCm, ToPoint, ToTangent);
			Scene.TruckTo = ToPoint + RightOfLevel(ToTangent.GetSafeNormal2D()) * TruckLateral;
		}
		else
		{
			// No circuit to go by: the car's own heading, its right-hand side.
			Scene.Along = FRotator(0.0f, Car.YawDeg, 0.0f).Vector().GetSafeNormal2D();
			Scene.Outward = RightOfLevel(Scene.Along);
			Scene.Post = Car.Location - Scene.Along * PostUpCm + Scene.Outward * PostMinLateralCm;
			Scene.TruckFrom = Car.Location - Scene.Along * TruckFromCm + Scene.Outward * TruckBesideCm;
			Scene.TruckTo = Car.Location - Scene.Along * TruckStopCm + Scene.Outward * TruckBesideCm;
		}
		Scene.Post.Z = Car.Location.Z;
		Scene.TruckFrom.Z = Car.Location.Z;
		Scene.TruckTo.Z = Car.Location.Z;
		return Scene;
	}

	void FTracker::Tick(TConstArrayView<FCarSample> Cars, bool bRacing, float DeltaSeconds, const ApexTv::FPath* Path)
	{
		if (!bRacing)
		{
			Reset();
			return;
		}
		DeltaSeconds = FMath::Max(DeltaSeconds, 0.0f);

		for (FIncident& Scene : Incidents)
		{
			Scene.Age += DeltaSeconds;
			if (Scene.GoneFor >= 0.0f)
			{
				Scene.GoneFor += DeltaSeconds;
			}
		}

		for (const FCarSample& Car : Cars)
		{
			FIncident* Scene = Incidents.FindByPredicate([&Car](const FIncident& S) { return S.CarIndex == Car.CarIndex; });
			const bool bStopped = Car.SpeedMps < StoppedMps && !Car.bInPits;
			FCarMemory* Known = Memory.Find(Car.CarIndex);
			FCarMemory& Mind = Known ? *Known : Memory.Add(Car.CarIndex, FCarMemory{0.0f, Car.Location, false});
			// Moving: a stop starts afresh. Put somewhere else while stopped
			// (towed to its box, which is outside the lane the server counts):
			// the car is where it was meant to go, and no one comes out to it
			// until it has driven again.
			if (!bStopped)
			{
				Mind.StillFor = 0.0f;
				Mind.bDismissed = false;
			}
			else if (FVector::Dist2D(Mind.Last, Car.Location) > MovedAwayCm)
			{
				Mind.StillFor = 0.0f;
				Mind.bDismissed = true;
			}
			if (bStopped)
			{
				Mind.StillFor += DeltaSeconds;
			}
			Mind.Last = Car.Location;
			const float Still = Mind.StillFor;

			if (Scene)
			{
				// Towed to its box (or put back on the road somewhere else), or
				// driving on: the marshals stop and the scene clears soon after.
				const bool bMoved = FVector::Dist2D(Car.Location, Scene->CarLocation) > MovedAwayCm;
				const bool bGone = bMoved || Car.bInPits || Car.SpeedMps > DrivingOnMps;
				if (bGone && Scene->GoneFor < 0.0f)
				{
					Scene->GoneFor = 0.0f;
				}
				continue;
			}

			const bool bOpen = bStopped && !Mind.bDismissed
				&& (((!Car.bOnTrack || Car.bOut) && Still >= OffRoadAfterS) || Still >= OnRoadAfterS);
			if (!bOpen)
			{
				continue;
			}
			if (Incidents.Num() >= MaxIncidents)
			{
				// The newest stop is the one on screen: drop a finished scene
				// first, else the oldest.
				int32 Drop = 0;
				for (int32 Index = 1; Index < Incidents.Num(); ++Index)
				{
					const bool bGoneHere = Incidents[Index].GoneFor >= 0.0f;
					const bool bGoneDrop = Incidents[Drop].GoneFor >= 0.0f;
					if ((bGoneHere && !bGoneDrop) || (bGoneHere == bGoneDrop && Incidents[Index].Age > Incidents[Drop].Age))
					{
						Drop = Index;
					}
				}
				if (FCarMemory* Dropped = Memory.Find(Incidents[Drop].CarIndex))
				{
					Dropped->bDismissed = true;
				}
				Incidents.RemoveAt(Drop);
			}
			Incidents.Add(Open(Car, Path));
		}

		// A car that is no longer in the field has gone too.
		for (FIncident& Scene : Incidents)
		{
			if (Scene.GoneFor < 0.0f && !Cars.ContainsByPredicate([&Scene](const FCarSample& C) { return C.CarIndex == Scene.CarIndex; }))
			{
				Scene.GoneFor = 0.0f;
			}
		}
		Incidents.RemoveAll([this](const FIncident& Scene)
		{
			if (Scene.GoneFor <= LingerS)
			{
				return false;
			}
			if (FCarMemory* Done = Memory.Find(Scene.CarIndex))
			{
				Done->bDismissed = true;
			}
			return true;
		});
	}
}
