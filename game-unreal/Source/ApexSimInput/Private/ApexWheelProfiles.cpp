#include "ApexWheelProfiles.h"

namespace ApexWheelProfiles
{
	namespace
	{
		// USB vendor ids, as the device reports them (DIPROP_VIDPID).
		constexpr uint16 VendorLogitech = 0x046D;
		constexpr uint16 VendorThrustmaster = 0x044F;
		constexpr uint16 VendorFanatec = 0x0EB7;
		constexpr uint16 VendorMoza = 0x346E;
		// Granite Devices' boards sit on a shared vendor id (MCS Electronics),
		// so a Simucube is only a Simucube when its name says so as well.
		constexpr uint16 VendorSimucube = 0x16D0;

		const FProfile Profiles[] = {
			{ TEXT("generic"),            TEXT("Generic"),                        EDrive::Unknown, 0.0f },

			{ TEXT("fanatec-csw-v25"),    TEXT("Fanatec ClubSport V2.5"),         EDrive::Belt,    8.0f },
			{ TEXT("fanatec-csw-v2"),     TEXT("Fanatec ClubSport V2"),           EDrive::Belt,    8.0f },
			{ TEXT("fanatec-csl-elite"),  TEXT("Fanatec CSL Elite"),              EDrive::Belt,    6.0f },
			// 8 Nm with the 180 W supply; the player says so if they have it.
			{ TEXT("fanatec-csl-dd"),     TEXT("Fanatec CSL DD"),                 EDrive::Direct,  5.0f },
			{ TEXT("fanatec-gt-dd-pro"),  TEXT("Fanatec GT DD Pro"),              EDrive::Direct,  5.0f },
			{ TEXT("fanatec-csw-dd"),     TEXT("Fanatec ClubSport DD"),           EDrive::Direct,  12.0f },
			{ TEXT("fanatec-dd1"),        TEXT("Fanatec Podium DD1"),             EDrive::Direct,  20.0f },
			{ TEXT("fanatec-dd2"),        TEXT("Fanatec Podium DD2"),             EDrive::Direct,  25.0f },

			{ TEXT("logitech-g25"),       TEXT("Logitech G25"),                   EDrive::Gear,    2.5f },
			{ TEXT("logitech-g27"),       TEXT("Logitech G27"),                   EDrive::Gear,    2.5f },
			{ TEXT("logitech-dfgt"),      TEXT("Logitech Driving Force GT"),      EDrive::Gear,    2.0f },
			{ TEXT("logitech-g29"),       TEXT("Logitech G29"),                   EDrive::Gear,    2.2f },
			{ TEXT("logitech-g920"),      TEXT("Logitech G920"),                  EDrive::Gear,    2.2f },
			{ TEXT("logitech-g923"),      TEXT("Logitech G923"),                  EDrive::Gear,    2.2f },
			{ TEXT("logitech-pro"),       TEXT("Logitech PRO Racing Wheel"),      EDrive::Direct,  11.0f },

			{ TEXT("thrustmaster-t150"),  TEXT("Thrustmaster T150"),              EDrive::Hybrid,  2.0f },
			{ TEXT("thrustmaster-tmx"),   TEXT("Thrustmaster TMX"),               EDrive::Hybrid,  2.0f },
			{ TEXT("thrustmaster-t248"),  TEXT("Thrustmaster T248"),              EDrive::Hybrid,  3.5f },
			{ TEXT("thrustmaster-t300"),  TEXT("Thrustmaster T300 RS"),           EDrive::Belt,    3.9f },
			{ TEXT("thrustmaster-tx"),    TEXT("Thrustmaster TX"),                EDrive::Belt,    3.9f },
			{ TEXT("thrustmaster-t500"),  TEXT("Thrustmaster T500 RS"),           EDrive::Belt,    4.5f },
			{ TEXT("thrustmaster-t-gt"),  TEXT("Thrustmaster T-GT"),              EDrive::Belt,    6.0f },
			{ TEXT("thrustmaster-ts-pc"), TEXT("Thrustmaster TS-PC Racer"),       EDrive::Belt,    6.0f },
			{ TEXT("thrustmaster-ts-xw"), TEXT("Thrustmaster TS-XW"),             EDrive::Belt,    6.0f },
			{ TEXT("thrustmaster-t598"),  TEXT("Thrustmaster T598"),              EDrive::Direct,  5.0f },
			{ TEXT("thrustmaster-t818"),  TEXT("Thrustmaster T818"),              EDrive::Direct,  10.0f },

			{ TEXT("moza-r3"),            TEXT("Moza R3"),                        EDrive::Direct,  3.9f },
			{ TEXT("moza-r5"),            TEXT("Moza R5"),                        EDrive::Direct,  5.5f },
			{ TEXT("moza-r9"),            TEXT("Moza R9"),                        EDrive::Direct,  9.0f },
			{ TEXT("moza-r12"),           TEXT("Moza R12"),                       EDrive::Direct,  12.0f },
			{ TEXT("moza-r16"),           TEXT("Moza R16"),                       EDrive::Direct,  16.0f },
			{ TEXT("moza-r21"),           TEXT("Moza R21"),                       EDrive::Direct,  21.0f },

			{ TEXT("simucube-2-sport"),   TEXT("Simucube 2 Sport"),               EDrive::Direct,  17.0f },
			{ TEXT("simucube-2-pro"),     TEXT("Simucube 2 Pro"),                 EDrive::Direct,  25.0f },
			{ TEXT("simucube-2-ultimate"), TEXT("Simucube 2 Ultimate"),           EDrive::Direct,  32.0f },
		};

		/**
		 * A device is a profile when its vendor id is the profile's and its
		 * name holds every token, in the normalised form (NormaliseName).
		 * First match wins, so a name that contains another's tokens comes
		 * before it ("CLUBSPORTDD" before the belt ClubSports, "V25" before
		 * "V2").
		 */
		struct FRule
		{
			const TCHAR* ProfileId;
			uint16 VendorId;
			const TCHAR* Tokens[2];
		};

		const FRule Rules[] = {
			{ TEXT("fanatec-csw-dd"),      VendorFanatec,      { TEXT("CLUBSPORTDD"), nullptr } },
			{ TEXT("fanatec-dd2"),         VendorFanatec,      { TEXT("DD2"), nullptr } },
			{ TEXT("fanatec-dd1"),         VendorFanatec,      { TEXT("DD1"), nullptr } },
			{ TEXT("fanatec-gt-dd-pro"),   VendorFanatec,      { TEXT("DDPRO"), nullptr } },
			{ TEXT("fanatec-csl-dd"),      VendorFanatec,      { TEXT("CSLDD"), nullptr } },
			{ TEXT("fanatec-csl-elite"),   VendorFanatec,      { TEXT("CSLELITE"), nullptr } },
			{ TEXT("fanatec-csw-v25"),     VendorFanatec,      { TEXT("CLUBSPORT"), TEXT("V25") } },
			{ TEXT("fanatec-csw-v2"),      VendorFanatec,      { TEXT("CLUBSPORT"), TEXT("V2") } },

			{ TEXT("logitech-g923"),       VendorLogitech,     { TEXT("G923"), nullptr } },
			{ TEXT("logitech-g920"),       VendorLogitech,     { TEXT("G920"), nullptr } },
			{ TEXT("logitech-g29"),        VendorLogitech,     { TEXT("G29"), nullptr } },
			{ TEXT("logitech-g27"),        VendorLogitech,     { TEXT("G27"), nullptr } },
			{ TEXT("logitech-g25"),        VendorLogitech,     { TEXT("G25"), nullptr } },
			{ TEXT("logitech-dfgt"),       VendorLogitech,     { TEXT("DRIVINGFORCEGT"), nullptr } },
			{ TEXT("logitech-pro"),        VendorLogitech,     { TEXT("PRORACINGWHEEL"), nullptr } },

			{ TEXT("thrustmaster-t818"),   VendorThrustmaster, { TEXT("T818"), nullptr } },
			{ TEXT("thrustmaster-t598"),   VendorThrustmaster, { TEXT("T598"), nullptr } },
			{ TEXT("thrustmaster-ts-xw"),  VendorThrustmaster, { TEXT("TSXW"), nullptr } },
			{ TEXT("thrustmaster-ts-pc"),  VendorThrustmaster, { TEXT("TSPC"), nullptr } },
			{ TEXT("thrustmaster-t-gt"),   VendorThrustmaster, { TEXT("TGT"), nullptr } },
			{ TEXT("thrustmaster-t500"),   VendorThrustmaster, { TEXT("T500"), nullptr } },
			{ TEXT("thrustmaster-t300"),   VendorThrustmaster, { TEXT("T300"), nullptr } },
			{ TEXT("thrustmaster-t248"),   VendorThrustmaster, { TEXT("T248"), nullptr } },
			{ TEXT("thrustmaster-t150"),   VendorThrustmaster, { TEXT("T150"), nullptr } },
			{ TEXT("thrustmaster-tmx"),    VendorThrustmaster, { TEXT("TMX"), nullptr } },
			{ TEXT("thrustmaster-tx"),     VendorThrustmaster, { TEXT("TXRACING"), nullptr } },

			{ TEXT("moza-r21"),            VendorMoza,         { TEXT("R21"), nullptr } },
			{ TEXT("moza-r16"),            VendorMoza,         { TEXT("R16"), nullptr } },
			{ TEXT("moza-r12"),            VendorMoza,         { TEXT("R12"), nullptr } },
			{ TEXT("moza-r9"),             VendorMoza,         { TEXT("R9"), nullptr } },
			{ TEXT("moza-r5"),             VendorMoza,         { TEXT("R5"), nullptr } },
			{ TEXT("moza-r3"),             VendorMoza,         { TEXT("R3"), nullptr } },

			{ TEXT("simucube-2-ultimate"), VendorSimucube,     { TEXT("SIMUCUBE"), TEXT("ULTIMATE") } },
			{ TEXT("simucube-2-pro"),      VendorSimucube,     { TEXT("SIMUCUBE"), TEXT("PRO") } },
			{ TEXT("simucube-2-sport"),    VendorSimucube,     { TEXT("SIMUCUBE"), TEXT("SPORT") } },
		};

		/** Upper case, letters and digits only: "FANATEC ClubSport Wheel Base V2.5" -> "FANATECCLUBSPORTWHEELBASEV25". */
		FString NormaliseName(const FString& Name)
		{
			FString Out;
			Out.Reserve(Name.Len());
			for (const TCHAR Char : Name)
			{
				if (FChar::IsAlnum(Char))
				{
					Out.AppendChar(FChar::ToUpper(Char));
				}
			}
			return Out;
		}
	}

	const TCHAR* DriveName(EDrive Drive)
	{
		switch (Drive)
		{
		case EDrive::Gear:   return TEXT("Gear");
		case EDrive::Hybrid: return TEXT("Gear + belt");
		case EDrive::Belt:   return TEXT("Belt");
		case EDrive::Direct: return TEXT("Direct drive");
		default:             return TEXT("Unknown");
		}
	}

	TConstArrayView<FProfile> All()
	{
		return MakeArrayView(Profiles);
	}

	const FProfile& Generic()
	{
		return Profiles[0];
	}

	const FProfile* Find(const FString& Id)
	{
		for (const FProfile& Profile : Profiles)
		{
			if (Id.Equals(Profile.Id, ESearchCase::IgnoreCase))
			{
				return &Profile;
			}
		}
		return nullptr;
	}

	int32 IndexOf(const FProfile& Profile)
	{
		const int32 Index = static_cast<int32>(&Profile - Profiles);
		return Index >= 0 && Index < UE_ARRAY_COUNT(Profiles) ? Index : INDEX_NONE;
	}

	const FProfile& Detect(const FString& ProductName, uint16 VendorId)
	{
		const FString Name = NormaliseName(ProductName);
		// Makers name their pedals, shifters and handbrakes after the base they
		// go with ("CSL Elite Pedals V2"); none of them is a wheelbase.
		for (const TCHAR* NotABase : { TEXT("PEDAL"), TEXT("SHIFTER"), TEXT("HANDBRAKE") })
		{
			if (Name.Contains(NotABase, ESearchCase::CaseSensitive))
			{
				return Generic();
			}
		}
		for (const FRule& Rule : Rules)
		{
			if (Rule.VendorId != VendorId)
			{
				continue;
			}
			bool bAll = true;
			for (const TCHAR* Token : Rule.Tokens)
			{
				if (Token && !Name.Contains(Token, ESearchCase::CaseSensitive))
				{
					bAll = false;
					break;
				}
			}
			if (bAll)
			{
				if (const FProfile* Profile = Find(Rule.ProfileId))
				{
					return *Profile;
				}
			}
		}
		return Generic();
	}

	float OutputScale(float PeakNm)
	{
		return FMath::IsFinite(PeakNm) && PeakNm > ReferencePeakTorqueNm ? ReferencePeakTorqueNm / PeakNm : 1.0f;
	}

	float MinimumForce(EDrive Drive)
	{
		switch (Drive)
		{
		case EDrive::Gear:   return 0.05f;
		case EDrive::Hybrid: return 0.03f;
		default:             return 0.0f;
		}
	}
}
