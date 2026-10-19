#pragma once

#include "CoreMinimal.h"

/**
 * The maths of a triple-monitor rig, kept free of anything that needs a
 * world so it can be pinned by tests.
 *
 * A single wide picture across three monitors is wrong twice over: a
 * rectilinear projection 150° wide stretches everything at its edges, and
 * the side monitors are not in the centre monitor's plane anyway — a rig
 * toes them in toward the driver. So each monitor gets its own view: the
 * centre one square to the eye, each side one yawed by the rig's angle and
 * *off-axis*, because the eye does not sit in front of the side panel's
 * middle. ApexSideView (one engine view per side, through split screen)
 * draws what SideView works out here.
 *
 * Frame: the driver's eye is the origin, +X is forward (into the centre
 * screen), +Y is right, as the engine has it. Everything is centimetres.
 */
namespace ApexMultiView
{
	/** One monitor of the three, as the player measured it. The three are taken to be alike. */
	struct FTripleGeometry
	{
		/** Visible width of one panel's picture. */
		float ScreenWidthCm = 60.0f;

		/**
		 * The gap between two neighbouring pictures: both monitors' bezels
		 * together. Nothing is drawn in it, so a car driving across the join
		 * disappears behind the bezel instead of jumping.
		 */
		float BezelCm = 2.0f;

		/** From the eye to the middle of the centre panel. */
		float EyeDistanceCm = 65.0f;

		/**
		 * How far each side panel is turned toward the driver, about the
		 * edge it shares with the centre panel: 0 is a flat row, 90 has the
		 * side panels facing each other.
		 */
		float SideAngleDeg = 45.0f;

		/** A panel's width over its height, in pixels: what decides the vertical field. */
		float AspectRatio = 16.0f / 9.0f;
	};

	constexpr float MinScreenWidthCm = 30.0f;
	constexpr float MaxScreenWidthCm = 150.0f;
	constexpr float MinBezelCm = 0.0f;
	constexpr float MaxBezelCm = 15.0f;
	constexpr float MinEyeDistanceCm = 30.0f;
	constexpr float MaxEyeDistanceCm = 200.0f;
	constexpr float MinSideAngleDeg = 0.0f;
	constexpr float MaxSideAngleDeg = 90.0f;

	/** Every figure inside its range; the aspect left alone. */
	APEXSIM_API FTripleGeometry Clamp(const FTripleGeometry& Geometry);

	/**
	 * The horizontal field the centre panel subtends from the eye, degrees:
	 * the only field of view a triple rig has, which is why the Camera
	 * page's slider does nothing while one is set up.
	 */
	APEXSIM_API float CentreFovDeg(const FTripleGeometry& Geometry);

	/**
	 * Where the eye would have to be for the centre panel to subtend this
	 * field. A broadcast camera's long lens is a narrower field than the
	 * rig's; the side views are then laid out as if the eye sat that far
	 * back, so the three stay one picture.
	 */
	APEXSIM_API float EyeDistanceForFov(const FTripleGeometry& Geometry, float CentreFovDeg);

	/** What a side panel's view is: a yaw off the centre view, and its frustum. */
	struct FSideView
	{
		/** The camera's yaw relative to the centre view, degrees; negative for the left panel. */
		float YawDeg = 0.0f;

		/** Horizontal field of a *symmetric* frustum the panel's width would subtend at its distance. */
		float FovDeg = 90.0f;

		/**
		 * How far the frustum is slid sideways, in halves of its own width
		 * (FMinimalViewInfo::OffCenterProjectionOffset.X): -2 is the panel
		 * immediately to the left of a flat row's centre panel.
		 */
		float OffCenterX = 0.0f;
	};

	/**
	 * The left or right panel's view when the centre panel is drawn at
	 * CentreFovDeg. With a flat row (angle 0) and no bezel, the three views
	 * are exactly one planar projection; with the panels toed in, each view
	 * is the perspective of its own panel, so a straight line crossing a
	 * join bends on screen exactly as the screens do.
	 */
	APEXSIM_API FSideView SideView(const FTripleGeometry& Geometry, float CentreFovDeg, bool bLeft);

	/** A monitor, for FindTripleRow: its desktop rectangle and whether it is the primary. */
	struct FMonitorRect
	{
		FIntRect Rect;
		bool bPrimary = false;
	};

	/**
	 * Finds three monitors of the same size standing in a row, edge to edge,
	 * and returns the desktop rectangle they make together. The row holding
	 * the primary monitor in the middle wins, then one holding it anywhere.
	 * False when the desktop has no such row: the game then lays its three
	 * views across whatever window it has, which is how the mode is checked
	 * on a machine with one monitor.
	 */
	APEXSIM_API bool FindTripleRow(const TArray<FMonitorRect>& Monitors, FIntRect& OutSpan);
}
