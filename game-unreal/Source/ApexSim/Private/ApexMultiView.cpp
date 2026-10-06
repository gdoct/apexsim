#include "ApexMultiView.h"

namespace ApexMultiView
{
	FTripleGeometry Clamp(const FTripleGeometry& Geometry)
	{
		FTripleGeometry Out = Geometry;
		Out.ScreenWidthCm = FMath::Clamp(Geometry.ScreenWidthCm, MinScreenWidthCm, MaxScreenWidthCm);
		Out.BezelCm = FMath::Clamp(Geometry.BezelCm, MinBezelCm, MaxBezelCm);
		Out.EyeDistanceCm = FMath::Clamp(Geometry.EyeDistanceCm, MinEyeDistanceCm, MaxEyeDistanceCm);
		Out.SideAngleDeg = FMath::Clamp(Geometry.SideAngleDeg, MinSideAngleDeg, MaxSideAngleDeg);
		return Out;
	}

	float CentreFovDeg(const FTripleGeometry& Geometry)
	{
		const FTripleGeometry G = Clamp(Geometry);
		return FMath::RadiansToDegrees(2.0f * FMath::Atan2(0.5f * G.ScreenWidthCm, G.EyeDistanceCm));
	}

	float EyeDistanceForFov(const FTripleGeometry& Geometry, float InCentreFovDeg)
	{
		const FTripleGeometry G = Clamp(Geometry);
		const float HalfFov = FMath::DegreesToRadians(FMath::Clamp(InCentreFovDeg, 1.0f, 175.0f) * 0.5f);
		return 0.5f * G.ScreenWidthCm / FMath::Tan(HalfFov);
	}

	FSideView SideView(const FTripleGeometry& Geometry, float InCentreFovDeg, bool bLeft)
	{
		const FTripleGeometry G = Clamp(Geometry);
		const float W = G.ScreenWidthCm;
		const float HalfGap = 0.5f * G.BezelCm;
		const float D = EyeDistanceForFov(G, InCentreFovDeg);
		const float Theta = FMath::DegreesToRadians(G.SideAngleDeg);
		const float SinT = FMath::Sin(Theta);
		const float CosT = FMath::Cos(Theta);

		// Worked for the left panel; the right one is its mirror image.
		//
		// The hinge — the point the side panel turns about — is the centre
		// panel's left edge plus half the gap, in the centre panel's plane:
		// h = (D, -(W/2 + gap/2)). The side panel leaves the hinge toward the
		// driver along dir = (-sin θ, -cos θ), its own half of the gap first
		// and then its W of picture. In the frame turned left by θ (forward'
		// = (cos θ, -sin θ), right' = (sin θ, cos θ)) that direction is
		// exactly -right', so the panel is a plane square to forward' at
		// distance D' = h·forward', spanning right' from x1 to x0:
		//   x0 = h·right' - gap/2   (the near end: the picture's inner edge)
		//   x1 = x0 - W             (the far end)
		// A symmetric frustum W wide at D' sees right' in [-W/2, W/2]; the
		// panel sits at [x1, x0], which is that frustum slid by its centre,
		// (x0 + x1)/2, measured in halves of its width.
		const float PlaneDistance = D * CosT + (0.5f * W + HalfGap) * SinT;
		const float NearEnd = D * SinT - (0.5f * W + HalfGap) * CosT - HalfGap;
		const float Centre = NearEnd - 0.5f * W;
		const float HalfWidth = 0.5f * W;

		FSideView View;
		View.YawDeg = bLeft ? -G.SideAngleDeg : G.SideAngleDeg;
		View.FovDeg = FMath::RadiansToDegrees(2.0f * FMath::Atan2(HalfWidth, PlaneDistance));
		View.OffCenterX = (bLeft ? 1.0f : -1.0f) * Centre / HalfWidth;
		return View;
	}

	bool FindTripleRow(const TArray<FMonitorRect>& Monitors, FIntRect& OutSpan)
	{
		// A few pixels of slack: Windows reports a row laid out by hand as
		// touching, but a DPI-scaled arrangement can be off by one or two.
		constexpr int32 Slack = 4;
		auto Touches = [](int32 A, int32 B) { return FMath::Abs(A - B) <= Slack; };

		int32 BestScore = -1;
		for (int32 Left = 0; Left < Monitors.Num(); ++Left)
		{
			for (int32 Middle = 0; Middle < Monitors.Num(); ++Middle)
			{
				for (int32 Right = 0; Right < Monitors.Num(); ++Right)
				{
					if (Left == Middle || Middle == Right || Left == Right)
					{
						continue;
					}
					const FIntRect& L = Monitors[Left].Rect;
					const FIntRect& M = Monitors[Middle].Rect;
					const FIntRect& R = Monitors[Right].Rect;
					if (L.Size() != M.Size() || M.Size() != R.Size() || M.Width() <= 0 || M.Height() <= 0)
					{
						continue;
					}
					if (!Touches(L.Min.Y, M.Min.Y) || !Touches(M.Min.Y, R.Min.Y)
						|| !Touches(L.Max.X, M.Min.X) || !Touches(M.Max.X, R.Min.X))
					{
						continue;
					}

					const int32 Score = Monitors[Middle].bPrimary ? 2
						: (Monitors[Left].bPrimary || Monitors[Right].bPrimary) ? 1 : 0;
					if (Score > BestScore)
					{
						BestScore = Score;
						OutSpan = FIntRect(L.Min.X, M.Min.Y, L.Min.X + 3 * M.Width(), M.Min.Y + M.Height());
					}
				}
			}
		}
		return BestScore >= 0;
	}
}
