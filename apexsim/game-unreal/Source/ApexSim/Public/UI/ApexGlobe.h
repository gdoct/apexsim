#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"

/**
 * The maths of Garage > Tracks' globe (UApexGlobeWidget), kept free of
 * widgets so it can be tested: an orthographic view of a unit sphere turned
 * to (Lon, Lat) and scaled by Zoom, how that view moves (fly-to, drag
 * inertia, idle spin, eased zoom), where a circuit's pin stands, and which
 * labels fit.
 *
 * Screen coordinates here are in globe radii about the globe's centre, Y up;
 * the widget scales them by its radius in pixels and flips Y.
 */
namespace ApexGlobe
{
	/** Where the view rests at start and after Reset. */
	inline constexpr double HomeLonDeg = -25.0;
	inline constexpr double HomeLatDeg = 22.0;
	inline constexpr double HomeZoom = 1.0;

	inline constexpr double MinZoom = 0.85;
	inline constexpr double MaxZoom = 5.0;
	/** What a selected circuit is flown to. */
	inline constexpr double SelectZoom = 2.4;
	/** One zoom button press, or a pad's trigger held for a second. */
	inline constexpr double ZoomStep = 1.35;
	/** The view never tips over a pole. */
	inline constexpr double LatLimitDeg = 85.0;
	inline constexpr double AutoRotateDegPerSec = 3.5;
	/** The pin's head above the ground, in radii. */
	inline constexpr double PinHeight = 0.085;
	/** A pin shows from this depth (the cosine to the viewer) and fades in over FadeSpan. */
	inline constexpr double PinMinDepth = 0.08;
	inline constexpr double PinFadeSpan = 0.25;
	/** A drag that rested this long before the release throws nothing. */
	inline constexpr double ThrowRestSeconds = 0.09;

	/** Longitude folded into -180..180. */
	APEXSIM_API double WrapLon(double LonDeg);

	/** A point on the unit sphere: (cos(lat) sin(lon), sin(lat), cos(lat) cos(lon)). */
	APEXSIM_API FVector3d Direction(double LonDeg, double LatDeg);

	struct APEXSIM_API FView
	{
		double LonDeg = HomeLonDeg;
		double LatDeg = HomeLatDeg;
		double Zoom = HomeZoom;
		/** The zoom the view eases to. */
		double TargetZoom = HomeZoom;
		/** A fly-to in progress, to TargetLon / TargetLat. */
		bool bFlying = false;
		double TargetLonDeg = 0.0;
		double TargetLatDeg = 0.0;
		/** Spin left from a throw, degrees a second. */
		double SpinLon = 0.0;
		double SpinLat = 0.0;
		bool bDragging = false;
		/** The idle spin waits while either holds: the pointer is over the globe, or a circuit is chosen. */
		bool bHovered = false;
		bool bSelected = false;
		/** A pad's stick, -1..1 each: turns the globe while held. */
		FVector2D Stick = FVector2D::ZeroVector;
		/** A pad's triggers, -1 (out) .. 1 (in). */
		double ZoomInput = 0.0;
	};

	/**
	 * Where (LonDeg, LatDeg) on a sphere of RadiusScale radii lands: X right,
	 * Y up in radii of the zoomed globe, Z the depth (above 0 faces the viewer).
	 */
	APEXSIM_API FVector3d Project(const FView& View, double LonDeg, double LatDeg, double RadiusScale = 1.0);

	/** Advances the view by Dt seconds; true when anything moved. */
	APEXSIM_API bool Step(FView& View, double Dt);

	/** Eases to a circuit: its place under the middle, at SelectZoom. */
	APEXSIM_API void FlyTo(FView& View, double LonDeg, double LatDeg, double Zoom = SelectZoom);
	/** Back where it started, nothing selected. */
	APEXSIM_API void Reset(FView& View);
	/** Multiplies the target zoom, clamped. */
	APEXSIM_API void ZoomBy(FView& View, double Factor);

	/** A press on the globe: stops any fly-to and throw. */
	APEXSIM_API void BeginDrag(FView& View);
	/**
	 * The pointer moved by DeltaPx (Y down) over Dt seconds, with the zoomed
	 * globe RadiusPx pixels across its radius: the ground under it follows.
	 */
	APEXSIM_API void Drag(FView& View, const FVector2D& DeltaPx, double RadiusPx, double Dt);
	/** The release; a drag that rested ThrowRestSeconds before it is not thrown. */
	APEXSIM_API void EndDrag(FView& View, double SecondsSinceLastMove);

	/** A pin's opacity at Depth: 0 behind the limb, 1 once well round the front. */
	APEXSIM_API double PinFade(double Depth);

	struct FLabel
	{
		/** Where the label would sit, in any one screen space. */
		FBox2D Rect = FBox2D(ForceInit);
		/** The pin's depth: the nearer ones are placed first. */
		double Depth = 0.0;
		/** Selected, hovered or focused: shown whatever it covers, and placed before the rest. */
		bool bAlways = false;
	};

	/**
	 * Which labels show: the always-shown first, then front-most first,
	 * skipping any that would overlap one already placed.
	 */
	APEXSIM_API TArray<bool> PlaceLabels(const TArray<FLabel>& Labels);

	/**
	 * The pin a pad's D-pad goes to from From in Direction (screen space, Y
	 * down): wholly ahead of it, the nearest by distance with a sideways
	 * miss counted double. INDEX_NONE when nothing is that way. Skips From's own index.
	 */
	APEXSIM_API int32 NearestToward(const TArray<FVector2D>& Points, const TArray<bool>& Usable, int32 FromIndex,
		const FVector2D& From, const FVector2D& Direction);

	/** One land dot: its longitude, radians, and its latitude's sine and cosine. */
	struct FLandDot
	{
		double LonRad = 0.0;
		double SinLat = 0.0;
		double CosLat = 1.0;
	};

	/** The land mask (ApexGlobeLand.h) as dots, built once. */
	APEXSIM_API const TArray<FLandDot>& LandDots();
}
