#include "UI/ApexGlobe.h"

#include "UI/ApexGlobeLand.h"

namespace ApexGlobe
{
	double WrapLon(double LonDeg)
	{
		return FMath::Fmod(FMath::Fmod(LonDeg + 540.0, 360.0) + 360.0, 360.0) - 180.0;
	}

	FVector3d Direction(double LonDeg, double LatDeg)
	{
		const double Lat = FMath::DegreesToRadians(LatDeg);
		const double Lon = FMath::DegreesToRadians(LonDeg);
		return FVector3d(FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat), FMath::Cos(Lat) * FMath::Cos(Lon));
	}

	FVector3d Project(const FView& View, double LonDeg, double LatDeg, double RadiusScale)
	{
		const double R = View.Zoom * RadiusScale;
		const double P = FMath::DegreesToRadians(LatDeg);
		const double DL = FMath::DegreesToRadians(LonDeg - View.LonDeg);
		const double P0 = FMath::DegreesToRadians(View.LatDeg);
		const double CP = FMath::Cos(P);
		const double SP = FMath::Sin(P);
		const double X = CP * FMath::Sin(DL);
		const double Y = FMath::Cos(P0) * SP - FMath::Sin(P0) * CP * FMath::Cos(DL);
		const double Z = FMath::Sin(P0) * SP + FMath::Cos(P0) * CP * FMath::Cos(DL);
		return FVector3d(R * X, R * Y, Z);
	}

	bool Step(FView& View, double Dt)
	{
		bool bMoved = false;
		if (!View.bDragging)
		{
			if (!View.Stick.IsNearlyZero(0.15))
			{
				// A pad turns the ground under the middle at a pace that
				// reads the same at every zoom.
				const double Rate = 70.0 / View.Zoom;
				View.bFlying = false;
				View.SpinLon = View.SpinLat = 0.0;
				View.LonDeg = WrapLon(View.LonDeg + View.Stick.X * Rate * Dt);
				View.LatDeg = FMath::Clamp(View.LatDeg + View.Stick.Y * Rate * Dt, -LatLimitDeg, LatLimitDeg);
				bMoved = true;
			}
			else if (View.bFlying)
			{
				const double K = 1.0 - FMath::Exp(-Dt * 5.0);
				const double DL = WrapLon(View.TargetLonDeg - View.LonDeg);
				const double DP = View.TargetLatDeg - View.LatDeg;
				View.LonDeg = WrapLon(View.LonDeg + DL * K);
				View.LatDeg += DP * K;
				if (FMath::Abs(DL) < 0.03 && FMath::Abs(DP) < 0.03)
				{
					View.LonDeg = View.TargetLonDeg;
					View.LatDeg = View.TargetLatDeg;
					View.bFlying = false;
				}
				bMoved = true;
			}
			else if (FMath::Abs(View.SpinLon) > 0.02 || FMath::Abs(View.SpinLat) > 0.02)
			{
				View.LonDeg = WrapLon(View.LonDeg + View.SpinLon * Dt);
				View.LatDeg = FMath::Clamp(View.LatDeg + View.SpinLat * Dt, -LatLimitDeg, LatLimitDeg);
				const double Decay = FMath::Exp(-Dt * 3.2);
				View.SpinLon *= Decay;
				View.SpinLat *= Decay;
				bMoved = true;
			}
			else if (!View.bSelected && !View.bHovered)
			{
				View.LonDeg = WrapLon(View.LonDeg + AutoRotateDegPerSec * Dt);
				bMoved = true;
			}
		}
		if (FMath::Abs(View.ZoomInput) > 0.1)
		{
			ZoomBy(View, FMath::Pow(ZoomStep, View.ZoomInput * Dt * 2.0));
		}
		const double DZ = View.TargetZoom - View.Zoom;
		if (FMath::Abs(DZ) > 0.0005)
		{
			View.Zoom += DZ * (1.0 - FMath::Exp(-Dt * 6.0));
			bMoved = true;
		}
		return bMoved;
	}

	void FlyTo(FView& View, double LonDeg, double LatDeg, double Zoom)
	{
		View.bFlying = true;
		View.TargetLonDeg = WrapLon(LonDeg);
		View.TargetLatDeg = FMath::Clamp(LatDeg, -LatLimitDeg, LatLimitDeg);
		View.TargetZoom = FMath::Clamp(Zoom, MinZoom, MaxZoom);
		View.SpinLon = View.SpinLat = 0.0;
	}

	void Reset(FView& View)
	{
		View.bSelected = false;
		FlyTo(View, HomeLonDeg, HomeLatDeg, HomeZoom);
	}

	void ZoomBy(FView& View, double Factor)
	{
		View.TargetZoom = FMath::Clamp(View.TargetZoom * Factor, MinZoom, MaxZoom);
	}

	void BeginDrag(FView& View)
	{
		View.bDragging = true;
		View.bFlying = false;
		View.SpinLon = View.SpinLat = 0.0;
	}

	void Drag(FView& View, const FVector2D& DeltaPx, double RadiusPx, double Dt)
	{
		if (!View.bDragging || RadiusPx <= 0.0)
		{
			return;
		}
		const double Seconds = FMath::Max(0.004, Dt);
		const double DLon = -DeltaPx.X / RadiusPx * 57.2958;
		const double DLat = DeltaPx.Y / RadiusPx * 57.2958;
		View.LonDeg = WrapLon(View.LonDeg + DLon);
		View.LatDeg = FMath::Clamp(View.LatDeg + DLat, -LatLimitDeg, LatLimitDeg);
		View.SpinLon = 0.6 * View.SpinLon + 0.4 * DLon / Seconds;
		View.SpinLat = 0.6 * View.SpinLat + 0.4 * DLat / Seconds;
	}

	void EndDrag(FView& View, double SecondsSinceLastMove)
	{
		View.bDragging = false;
		if (SecondsSinceLastMove > ThrowRestSeconds)
		{
			View.SpinLon = View.SpinLat = 0.0;
		}
	}

	double PinFade(double Depth)
	{
		return FMath::Clamp((Depth - PinMinDepth) / PinFadeSpan, 0.0, 1.0);
	}

	TArray<bool> PlaceLabels(const TArray<FLabel>& Labels)
	{
		TArray<int32> Order;
		for (int32 i = 0; i < Labels.Num(); ++i)
		{
			Order.Add(i);
		}
		Order.StableSort([&Labels](int32 A, int32 B)
		{
			if (Labels[A].bAlways != Labels[B].bAlways)
			{
				return Labels[A].bAlways;
			}
			return Labels[A].Depth > Labels[B].Depth;
		});

		TArray<bool> Shown;
		Shown.Init(false, Labels.Num());
		TArray<FBox2D> Placed;
		for (const int32 i : Order)
		{
			const FBox2D& R = Labels[i].Rect;
			const bool bHit = Placed.ContainsByPredicate([&R](const FBox2D& Q)
			{
				return R.Min.X < Q.Max.X && R.Max.X > Q.Min.X && R.Min.Y < Q.Max.Y && R.Max.Y > Q.Min.Y;
			});
			if (!bHit || Labels[i].bAlways)
			{
				Placed.Add(R);
				Shown[i] = true;
			}
		}
		return Shown;
	}

	int32 NearestToward(const TArray<FVector2D>& Points, const TArray<bool>& Usable, int32 FromIndex,
		const FVector2D& From, const FVector2D& Direction)
	{
		const FVector2D Ahead = Direction.GetSafeNormal();
		const FVector2D Side(-Ahead.Y, Ahead.X);
		int32 Best = INDEX_NONE;
		double BestScore = TNumericLimits<double>::Max();
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			if (i == FromIndex || !Usable.IsValidIndex(i) || !Usable[i])
			{
				continue;
			}
			const FVector2D D = Points[i] - From;
			const double Along = FVector2D::DotProduct(D, Ahead);
			const double Across = FMath::Abs(FVector2D::DotProduct(D, Side));
			// Within 60° of the way asked for: a pin far off to the side is
			// another direction's.
			if (Along <= 1.0 || Across > Along * 1.75)
			{
				continue;
			}
			const double Score = Along + 2.0 * Across;
			if (Score < BestScore)
			{
				BestScore = Score;
				Best = i;
			}
		}
		return Best;
	}

	const TArray<FLandDot>& LandDots()
	{
		static const TArray<FLandDot> Dots = []
		{
			TArray<FLandDot> Out;
			for (int32 j = 0; j < ApexGlobeLand::RowCount; ++j)
			{
				const char* Row = ApexGlobeLand::Rows[j];
				const int32 N = FCStringAnsi::Strlen(Row);
				const double Lat = FMath::DegreesToRadians(-90.0 + (j + 0.5) * ApexGlobeLand::BandDeg);
				for (int32 i = 0; i < N; ++i)
				{
					if (Row[i] == '1')
					{
						FLandDot Dot;
						Dot.LonRad = FMath::DegreesToRadians(-180.0 + (i + 0.5) * 360.0 / N);
						Dot.SinLat = FMath::Sin(Lat);
						Dot.CosLat = FMath::Cos(Lat);
						Out.Add(Dot);
					}
				}
			}
			return Out;
		}();
		return Dots;
	}
}
