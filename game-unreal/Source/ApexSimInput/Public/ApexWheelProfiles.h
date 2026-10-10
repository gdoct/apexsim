#pragma once

#include "CoreMinimal.h"

/**
 * What the game knows about a wheelbase model: how strong its motor is and
 * how it drives the rim.
 *
 * DirectInput forces are fractions of the device's own peak, so without this
 * the same Force setting asks a 2 Nm gear drive and a 25 Nm direct drive for
 * the same share of their motor: a corner that is 5 Nm in the hands on the
 * 8 Nm base the feel was tuned on (a ClubSport V2.5) is 15 Nm on the direct
 * drive, and the curb ribs, damper and soft lock scale up with it. A
 * profile's peak torque turns that back into newton-metres: a base stronger
 * than the reference plays everything at reference / peak, so the rim weighs
 * what it does on the reference base and the rest of the motor is headroom.
 * A weaker base keeps the fractions it has always had (it cannot give more
 * than it has; the Force slider is how to ask it for more of what it has).
 *
 * Profiles are matched on the USB vendor id the device reports and its
 * product name, and the player can choose another one or give the peak
 * themselves on the Wheel page (UApexSettingsSave::WheelProfiles) — the peak
 * is what the base's own software is set to, which is often less than the
 * box says. A device nothing matches gets Generic, which is exactly the
 * mixing the game did before profiles existed.
 *
 * Peak torques are the makers' published figures where there is one and
 * the commonly measured figure where there is not (Logitech, Thrustmaster's
 * belt and hybrid bases). Pure and platform-independent, covered by
 * `ApexSim.Input.WheelProfiles.*`.
 */
namespace ApexWheelProfiles
{
	/** How the motor turns the rim, which decides how much friction is in the way. */
	enum class EDrive : uint8
	{
		Unknown,
		/** Plastic gears: heavy friction and a notch around the centre that small forces cannot get through. */
		Gear,
		/** A gear stage and a belt (Thrustmaster's T150, TMX, T248). */
		Hybrid,
		Belt,
		Direct,
	};

	/** "Gear", "Belt", "Direct drive". */
	APEXSIMINPUT_API const TCHAR* DriveName(EDrive Drive);

	/** The base the force feedback's numbers were measured on: its peak is where output scaling starts. */
	inline constexpr float ReferencePeakTorqueNm = 8.0f;

	/** The range the Wheel page offers for a player's own peak figure. */
	inline constexpr float MinPeakTorqueNm = 1.0f;
	inline constexpr float MaxPeakTorqueNm = 40.0f;

	struct FProfile
	{
		/** Stable: written into the settings save. */
		const TCHAR* Id = TEXT("");
		/** What the Wheel page shows. */
		const TCHAR* DisplayName = TEXT("");
		EDrive Drive = EDrive::Unknown;
		/** Newton-metres at 100% of the base's own strength setting; 0 when unknown. */
		float PeakTorqueNm = 0.0f;
	};

	/** Every profile, in the order the Wheel page steps through them; Generic is first. */
	APEXSIMINPUT_API TConstArrayView<FProfile> All();

	/** Today's mixing, unchanged: no peak, no minimum force. */
	APEXSIMINPUT_API const FProfile& Generic();

	/** The profile with this id; nullptr for an id this build does not know. */
	APEXSIMINPUT_API const FProfile* Find(const FString& Id);

	/** The index of a profile in All(); INDEX_NONE when it is not one of them. */
	APEXSIMINPUT_API int32 IndexOf(const FProfile& Profile);

	/**
	 * The profile for an attached device, from its USB vendor id and the
	 * product name DirectInput reports; Generic when nothing matches. Names
	 * are compared with case, spaces and punctuation dropped, so "T-GT II"
	 * and "TGT II" are the same base.
	 */
	APEXSIMINPUT_API const FProfile& Detect(const FString& ProductName, uint16 VendorId);

	/**
	 * What every force the mixer sends is multiplied by on a base this strong:
	 * ReferencePeakTorqueNm / PeakNm above the reference, 1 at or below it and
	 * for an unknown peak (0).
	 */
	APEXSIMINPUT_API float OutputScale(float PeakNm);

	/**
	 * Least constant force, as a share of the base, that the rim feels: the
	 * mixer lifts any force off zero to at least this, so a light aligning
	 * torque is not lost in a gear drive's friction. 0 for a belt or a direct
	 * drive, whose friction the force feedback was tuned against.
	 */
	APEXSIMINPUT_API float MinimumForce(EDrive Drive);
}
