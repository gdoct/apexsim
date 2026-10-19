#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "ApexProtocolTypes.generated.h"

/**
 * ApexSim wire protocol v2 — mirrors `server/src/network.rs` and `sim/src/data.rs`.
 *
 * Framing: [4-byte big-endian length][MessagePack payload], `to_vec_named`.
 *
 * Both ClientMessage and ServerMessage are Rust enums declared
 * `#[serde(tag = "type", content = "data")]` — ADJACENTLY TAGGED. Every message
 * is therefore a map `{"type": "<Variant>", "data": <payload>}`, and a unit
 * variant is a ONE-KEY map with no "data" key at all. Emitting `"data": {}` for
 * a unit variant fails `from_slice` on the server, which charges 10 rate-limit
 * violations per attempt (200 => forced disconnect).
 *
 * ==== THE CASE ASYMMETRY — the single most error-prone part of this port ====
 *
 * The enums carry no `rename_all`, so fields declared INLINE on a variant stay
 * snake_case:
 *     Authenticate { token, player_name, protocol_version, resume_token? }
 *     AuthFailure { reason } / HeartbeatAck { server_tick } / Error { code, message }
 *
 * But the standalone payload STRUCTS carry `#[serde(rename_all = "PascalCase")]`,
 * so their fields are PascalCase:
 *     AuthSuccessData { PlayerId, ServerVersion, ProtocolVersion, UdpToken, UdpPort, ResumeToken? }
 *     LobbyStateData  { PlayersInLobby, AvailableSessions, CarConfigs, TrackConfigs }
 *     SessionJoinedData { SessionId, YourGridPosition, SessionKind, AllowedAssists, Conditions }
 *
 * AllowedAssists is a struct with NO rename_all, so its own keys are
 * snake_case ("abs", "racing_line") even under SessionJoinedData's PascalCase
 * "AllowedAssists" key — the TrackPoint situation again. SessionConditions
 * ("weather", "time_of_day_minutes") is the same under "Conditions".
 *
 * And one exception breaks even that: TrackPoint (network.rs:318-322) has NO
 * rename_all, so its keys are lowercase "x"/"y" while nested inside a
 * PascalCase parent.
 *
 * Because the parsers skip unknown keys (for forward compatibility), getting a
 * key's case wrong does not error — it silently yields an empty list. That is
 * why ProtocolCodecTests pins every message against a golden byte blob.
 */

/** Wire protocol version. The server rejects anything but this (network.rs:32). */
inline constexpr uint8 APEXSIM_PROTOCOL_VERSION = 2;

/** Mirrors `SessionState` (data.rs:888). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexSessionState : uint8
{
	Lobby     = 0,
	Countdown = 1,
	Racing    = 2,
	Finished  = 3,
};

/** Mirrors `SessionKind` (data.rs:897). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexSessionKind : uint8
{
	Multiplayer = 0,
	Practice    = 1,
	// 2 was Sandbox, which had no behaviour; never reuse it.
	/**
	 * An AI-only race this client watches behind its menu: unlisted and
	 * unjoinable. Created by UApexDemoModeSubsystem; the net subsystem keeps
	 * it out of every session delegate (see IsInDemoSession).
	 */
	Demo        = 3,
	/**
	 * One AI car lapping the track alone, for this client to watch (Main menu
	 * > Garage > Tracks > Watch hotlap). Unlisted like a demo, but seated as a
	 * live session's spectator, with its timing lines.
	 */
	HotlapWatch = 4,
};

/** Mirrors `TractionControl` (data.rs). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexTractionControl : uint8
{
	Off  = 0,
	/** Cuts drive only when a driven wheel would spin. */
	Low  = 1,
	/** Cuts drive as soon as the tyre's combined grip runs out, keeping a margin for cornering. */
	High = 2,
};

/**
 * Mirrors `DamageLevel` (data.rs): how much damage the cars in a session
 * take, a rule its host picks on create (CreateSession) and the same for
 * every car, AI included; SessionJoined echoes it. Serialize_repr => a plain
 * u8 on the wire, left off when Full. Scales every accrual on the server
 * (hits, overheating, over-revving); what damage costs is unchanged.
 */
UENUM(BlueprintType)
enum class EApexDamageLevel : uint8
{
	Off     = 0,
	/** Half of every hit. */
	Reduced = 1,
	Full    = 2,
	/** Dents and lost parts drawn as under Full; nothing costs the car anything. */
	VisualOnly = 3,
};

/**
 * How qualifying runs (data.rs `QualifyingFormat`), as CreateSession's
 * `qualifying_format` and SessionJoined's `QualifyingFormat` carry it:
 * Serialize_repr => a plain u8, left off the wire when Open.
 */
UENUM(BlueprintType)
enum class EApexQualifyingFormat : uint8
{
	/** Every car out when it likes; a weekend's rounds a timed knockout. */
	Open     = 0,
	/** One car at a time, an outlap and one flying lap each (a weekend's final round). */
	Shootout = 1,
};

/**
 * The qualifying lap allowance (CreateSession `qualifying_lap_allowance`):
 * the most flying laps a car may start in a qualifying session or round;
 * 0 for no limit.
 */
namespace ApexQualifyingLapAllowance
{
	inline constexpr int32 None = 0;
	inline constexpr int32 MaxLaps = 30;

	/** As the server keeps it: 0..MaxLaps. */
	inline int32 Clamp(int32 Laps)
	{
		return FMath::Clamp(Laps, None, MaxLaps);
	}

	/**
	 * The timed laps a car has left under an allowance of `Allowance` after
	 * ending `Run` (`update_lap_allowance`: each timed lap ended counts, the
	 * outlap does not); -1 for no allowance.
	 */
	inline int32 LapsLeft(int32 Allowance, int32 Run)
	{
		const int32 Laps = Clamp(Allowance);
		return Laps == None ? -1 : FMath::Max(0, Laps - FMath::Max(0, Run));
	}
}

/**
 * The AI field's level, as CreateSession's `ai_skill` and SessionJoined's
 * `AiSkill` carry it (ai_driver.rs `field_skills`): the skill the field is
 * spread round, Min to Max, or Mixed, the server's field of every level
 * from novice to ace, which is left off the wire. A rule of the session,
 * picked by its host on create.
 */
namespace ApexAiSkill
{
	constexpr int32 Mixed = -1;
	/** `MIN_SKILL_LEVEL` / `MAX_SKILL_LEVEL`. */
	constexpr int32 Min = 70;
	constexpr int32 Max = 110;
	/** Skill points between a field's slowest and quickest (`AI_FIELD_SPREAD`). */
	constexpr int32 Spread = 4;

	/** A level inside the bounds; anything negative is the mixed field. */
	inline int32 Clamp(int32 Skill)
	{
		return Skill < 0 ? Mixed : FMath::Clamp(Skill, Min, Max);
	}

	/** One step of the create screen's stepper: Mixed sits below Min. */
	inline int32 Step(int32 Skill, int32 Delta)
	{
		const int32 Current = Clamp(Skill);
		if (Current == Mixed)
		{
			return Delta > 0 ? Min : Mixed;
		}
		const int32 Next = Current + Delta;
		return Next < Min ? Mixed : FMath::Min(Next, Max);
	}

	/** What a level is called, after ai_driver.rs's bands; "Mixed" for the mixed field. */
	inline const TCHAR* Label(int32 Skill)
	{
		const int32 Level = Clamp(Skill);
		return Level == Mixed ? TEXT("Mixed")
			: Level <= 80     ? TEXT("Novice")
			: Level <= 90     ? TEXT("Amateur")
			: Level <= 100    ? TEXT("Pro")
			: Level <= 105    ? TEXT("Expert")
			:                   TEXT("Alien");
	}
}

/**
 * A timed race's length, as CreateSession's `race_seconds`, SessionJoined's
 * `RaceSeconds` and SessionSummary's `RaceSeconds` carry it (data.rs
 * `clamp_race_seconds`): seconds from the green light, 0 for a race over
 * laps, which is left off the wire. When the clock runs out the leader's
 * lap is the last.
 */
namespace ApexRaceLength
{
	/** `MIN_RACE_SECONDS` / `MAX_RACE_SECONDS`. */
	constexpr int32 MinSeconds = 60;
	constexpr int32 MaxSeconds = 24 * 3600;

	/** The create screen's stepper, in minutes, shortest first. */
	inline const TArray<int32>& LadderMinutes()
	{
		static const TArray<int32> Ladder = {
			5, 10, 15, 20, 25, 30, 40, 45, 60, 75, 90, 120, 150, 180, 240, 300, 360, 480, 600, 720, 1080, 1440 };
		return Ladder;
	}

	/** Seconds the server would keep: 0 (laps) for none, else inside the bounds. */
	inline int32 Clamp(int32 Seconds)
	{
		return Seconds <= 0 ? 0 : FMath::Clamp(Seconds, MinSeconds, MaxSeconds);
	}

	/** One step of the ladder from Minutes, held at either end; a value off the ladder steps to its neighbour. */
	inline int32 StepMinutes(int32 Minutes, int32 Delta)
	{
		const TArray<int32>& Ladder = LadderMinutes();
		if (Delta > 0)
		{
			for (const int32 Rung : Ladder)
			{
				if (Rung > Minutes) { return Rung; }
			}
			return Ladder.Last();
		}
		for (int32 Index = Ladder.Num() - 1; Index >= 0; --Index)
		{
			if (Ladder[Index] < Minutes) { return Ladder[Index]; }
		}
		return Ladder[0];
	}

	/** "45 min", "2 h", "1 h 30", "24 h": a length the way the menus say it. */
	inline FString Describe(int32 Seconds)
	{
		const int32 Minutes = FMath::Max(0, (Seconds + 30) / 60);
		const int32 Hours = Minutes / 60;
		const int32 Rest = Minutes % 60;
		if (Hours == 0)
		{
			return FString::Printf(TEXT("%d min"), Minutes);
		}
		return Rest == 0 ? FString::Printf(TEXT("%d h"), Hours) : FString::Printf(TEXT("%d h %02d"), Hours, Rest);
	}
}

/**
 * A timed qualifying session's length, as CreateSession's
 * `qualifying_seconds` and SessionJoined's `QualifyingSeconds` carry it
 * (data.rs `clamp_qualifying_seconds`): seconds from the moment qualifying
 * begins, 0 for open qualifying (no clock, no flag), which is left off the
 * wire. When the clock runs out every car on track takes the chequered flag
 * at its next crossing of the line, and the session finishes.
 */
namespace ApexQualifyingLength
{
	/** `MIN_QUALIFYING_SECONDS` / `MAX_QUALIFYING_SECONDS`. */
	constexpr int32 MinSeconds = 60;
	constexpr int32 MaxSeconds = 2 * 3600;

	/** The create screen's choices, minutes; 0 is open qualifying. */
	inline const TArray<int32>& ChoiceMinutes()
	{
		static const TArray<int32> Choices = { 0, 10, 15, 20, 30, 60 };
		return Choices;
	}

	/** Seconds the server would keep: 0 (open) for none, else inside the bounds. */
	inline int32 Clamp(int32 Seconds)
	{
		return Seconds <= 0 ? 0 : FMath::Clamp(Seconds, MinSeconds, MaxSeconds);
	}
}


/**
 * The qualifying cut-off (data.rs `clamp_qualifying_cutoff`): a car whose
 * best qualifying lap is slower than this share of pole, or that set none,
 * does not start the race. 0 is none; the create screen offers 107.
 */
namespace ApexQualifyingCutoff
{
	inline constexpr int32 None = 0;
	inline constexpr int32 Rule107 = 107;
	inline constexpr int32 MinPct = 101;
	inline constexpr int32 MaxPct = 150;

	/** As the server keeps it: 0 stays none, anything else in 101..150. */
	inline int32 Clamp(int32 Pct)
	{
		return Pct <= 0 ? None : FMath::Clamp(Pct, MinPct, MaxPct);
	}
}

/**
 * The race's pit-stop rule (data.rs `PitRule`): the stops every car must
 * make, at most MaxStops, and whether it must run a second dry compound. A
 * car that takes the flag owing either is classified behind every car that
 * met it (FApexRosterEntry::bMissedStops).
 */
namespace ApexMandatoryStops
{
	inline constexpr int32 None = 0;
	inline constexpr int32 MaxStops = 3;

	/** As the server keeps it: 0..MaxStops. */
	inline int32 Clamp(int32 Stops)
	{
		return FMath::Clamp(Stops, None, MaxStops);
	}

	/** The stops a car that has made `Made` still owes (`PitRule::owed`); 0 under no rule. */
	inline int32 Owed(int32 Stops, int32 Made)
	{
		return FMath::Max(0, Clamp(Stops) - FMath::Max(0, Made));
	}

	/** "1 stop", "2 stops · two compounds", "two compounds"; empty for no rule. */
	inline FString Describe(int32 Stops, bool bCompoundChange)
	{
		TArray<FString> Parts;
		const int32 Count = Clamp(Stops);
		if (Count > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d stop%s"), Count, Count == 1 ? TEXT("") : TEXT("s")));
		}
		if (bCompoundChange)
		{
			Parts.Add(TEXT("two compounds"));
		}
		return FString::Join(Parts, TEXT(" · "));
	}
}

/** `CompactCarState::ground`'s values a wheel (network.rs `GROUND_*`). */
namespace ApexGround
{
	inline constexpr uint8 None = 0;
	inline constexpr uint8 Gravel = 1;
	inline constexpr uint8 Sand = 2;
	/** A desert circuit's packed floor. */
	inline constexpr uint8 Desert = 3;
}

/** Mirrors the weekend phase byte of `WeekendState` (network.rs). */
UENUM(BlueprintType)
enum class EApexWeekendPhase : uint8
{
	Lobby      = 0,
	Practice   = 1,
	Qualifying = 2,
	Race       = 3,
};

/** A car knocked out of qualifying: where it starts, and the time that put it there. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexKnockedOut
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 CarIndex = -1;

	/**
	 * Joined after qualifying began: never ran a round, out with no time and
	 * on the grid behind every car that has one (`KnockedOutData.Late`).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bLate = false;

	/** The round it went out in, 1-based. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 Round = 0;

	/** Its grid slot, 1-based. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 GridPosition = 0;

	/** Its best legal lap in that round, ms; 0 for no time. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 LapTimeMs = 0;
};

/**
 * A one-lap qualifying shoot-out's run order and where it stands
 * (`ShootoutState`, sent on join and whenever it changes while one runs).
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexShootoutState
{
	GENERATED_BODY()

	/** False until the session sends one (no shoot-out, or an older server). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	bool bValid = false;

	/** The cars' indices (as the roster maps them) in run order, first out first. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	TArray<int32> Order;

	/** The 1-based place in Order of the car on its run; 0 when none is. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	int32 Running = 0;

	/** How many cars of Order have had their run. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	int32 Done = 0;

	/** The car on its run has started its flying lap. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	bool bFlying = false;

	/** A car's 1-based place in the run order; 0 when it is not in it. */
	int32 PlaceOf(int32 CarIndex) const { return Order.Find(CarIndex) + 1; }
};

/**
 * Mirrors `CarSetup` (car_setup.rs): the garage setup as *clicks* per knob,
 * one signed integer each, so neither the wire nor the client needs the
 * car's base figures — the server turns "+2" into newtons per metre against
 * the car.toml it loaded. Every knob has a fixed click range and a fixed
 * effect per click (ApexCarSetup::Knob); the server clamps whatever it is
 * sent, and all-zero is the car as filed. Encoded inline on SetCarSetup, so
 * the keys are snake_case; negative clicks are msgpack negative fixints.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarSetup
{
	GENERATED_BODY()

	static constexpr int32 KnobCount = 31;

	/** Clicks per knob, in ApexCarSetup::EKnob order. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Setup")
	TArray<int32> Clicks;

	FApexCarSetup()
	{
		Clicks.Init(0, KnobCount);
	}

	int32 GetClick(int32 Knob) const
	{
		return Clicks.IsValidIndex(Knob) ? Clicks[Knob] : 0;
	}

	/** Pins the value into the knob's range; returns whether anything changed. */
	bool SetClick(int32 Knob, int32 Value);

	/** Every knob pinned into range, as the server will do to it anyway. */
	void Clamp();

	/** True when every knob is at the file's value. */
	bool IsStock() const;

	/** How many knobs are away from the file's value. */
	int32 CountChanged() const;

	bool operator==(const FApexCarSetup& Other) const { return Clicks == Other.Clicks; }
	bool operator!=(const FApexCarSetup& Other) const { return !(*this == Other); }
};

namespace ApexCarSetup
{
	/** One knob of the setup: its wire key, click range and what a click does. */
	struct FKnob
	{
		/** The serde field name on the wire. */
		const ANSICHAR* Key;
		int32 Min;
		int32 Max;
		/** The size of one click for the read-out: "+2  (+8%)". */
		float PerClick;
		/** Unit of PerClick: "%", " kPa", " rpm". */
		const TCHAR* Unit;
	};

	/** The knobs in wire order — the same order as the server's `KNOBS`. */
	APEXSIMNET_API const FKnob& Knob(int32 Index);

	/** The read-out for a knob at a click count, e.g. "+2  (+8%)" or "0". */
	APEXSIMNET_API FString Describe(int32 Index, int32 Clicks);

	enum EKnob : int32
	{
		TyrePressureFront = 0,
		TyrePressureRear,
		RevLimiter,
		EngineBraking,
		FinalDrive,
		GearSpread,
		TorqueMap,
		BrakeBias,
		SpringFront,
		SpringRear,
		DamperFront,
		DamperRear,
		AntiRollFront,
		AntiRollRear,
		/** Laps of fuel over or under what the session fills the car with. */
		FuelLoad,
		/** Wing angle per axle: downforce, and drag, per click. */
		FrontWing,
		RearWing,
		/** Static ride height per axle (server `aero.rs`). */
		RideHeightFront,
		RideHeightRear,
		/** The compound of the next set: -1 hard, 0 medium, +1 soft. */
		TyreCompound,
		/** Front brake duct size: cooler brakes for a little drag. */
		BrakeDucts,
		/** Static camber per axle, degrees: negative for grip, positive for stability. */
		CamberFront,
		CamberRear,
		/** Toe per wheel, degrees: toe-in steadies the car but costs grip and tire heat. */
		ToeFront,
		ToeRear,
		/** Rear brake duct size, like the front's (server brakes.rs). */
		BrakeDuctsRear,
		/** The pad compound: -1 Endurance, 0 Standard, +1 Sprint; read out by name. */
		BrakePads,
		/** Radiator inlet: a cooler engine for a little drag per click open. */
		Radiator,
		/** The hybrid's mode at the start of a run: -1 Harvest, 0 Balanced, +1 Attack; read out by name. */
		ErsStartMode,
		/** The motor's recovery power under braking and off the throttle (server hybrid.rs). */
		ErsRegen,
		/** Where Balanced mode spends the lap budget: + earlier in the lap, - later; read out by name. */
		ErsDeployMap,
	};
	static_assert(ErsDeployMap + 1 == FApexCarSetup::KnobCount, "knob table and enum disagree");
}

/**
 * One setup knob in real units (`SetupKnobFigure`, setup_sheet.rs): its value
 * at a click count is `Stock + Clicks * Step`, held to Lo..Hi, printed with
 * Decimals and Unit.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSetupKnobFigure
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float Stock = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float Step = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float Lo = -MAX_flt;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float Hi = MAX_flt;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	int32 Decimals = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	FString Unit;
};

/**
 * Mirrors `CarSetupSheetData` (setup_sheet.rs): the garage's reference card
 * for the joining driver's car, sent once after SessionJoined. Knobs is in
 * ApexCarSetup::EKnob order; empty from a server that predates it.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarSetupSheet
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	FString CarConfigId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	TArray<FApexSetupKnobFigure> Knobs;

	/** Forward gears, first first, and the final drive, as filed. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	TArray<float> GearRatios;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float FinalDrive = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float WheelRadiusM = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float TyreOptimalPsi = 0.0f;

	/** Litres a lap of the session's track costs the car; 0 when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float LapFuelL = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float FuelKgPerL = 0.745f;

	/** Laps a hotlap fills the tank with, before the fuel knob. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float FillLaps = 3.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	bool bCamberModelled = false;

	/** Front downforce share moved per mm of rake over the stock rake. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float RakeBalancePerMm = 0.0f;

	/**
	 * The car's compound names in the order telemetry's `compound` byte and
	 * `PitService.compound` index them (`Compounds`); empty from a server that
	 * predates it, when the five defaults (soft, medium, hard, intermediate,
	 * wet) apply. The `tyre_compound` knob at zero picks ReferenceCompound and
	 * each click down the next in the list, so click = reference - index.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	TArray<FString> Compounds;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	int32 ReferenceCompound = 1;

	/**
	 * The hybrid as filed: battery, kWh (0: the car has none), motor power,
	 * kW, lap budget, kJ (0: none) and the speed the motor may drive from,
	 * km/h. Absent from a server that predates them, or for a car without one.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float HybridBatteryKwh = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float HybridMotorKw = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float HybridLapBudgetKj = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Setup")
	float HybridDeployMinKph = 0.0f;

	/** Whether the sheet named the car's compounds. */
	bool HasCompounds() const { return Compounds.Num() > 0; }

	/** Whether the car has a hybrid, so the ERS knobs mean anything. */
	bool HasHybrid() const { return HybridBatteryKwh > 0.0f; }

	/** The compound at a list index, upper-cased as the garage shows it; empty when out of range. */
	FString CompoundNameAt(int32 Index) const
	{
		return Compounds.IsValidIndex(Index) ? Compounds[Index].ToUpper() : FString();
	}

	/** The compound the knob picks at a click count: index = reference - clicks. */
	int32 CompoundIndexForClicks(int32 Clicks) const { return ReferenceCompound - Clicks; }
	int32 ClicksForCompoundIndex(int32 Index) const { return ReferenceCompound - Index; }
};

/**
 * Mirrors `AllowedAssists` (data.rs): which driving aids a session's host lets
 * its drivers use, fixed when the session is created. Sent inside
 * CreateSession and echoed in SessionJoined; the server forces a disallowed
 * aid off for every driver, so the client only has to show the lock. Every
 * field is true from a server that predates it.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexAllowedAssists
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bAbs = true;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bTractionControl = true;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bAutoGearbox = true;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bSteeringAssist = true;

	/** The server sends no RacingLine at all when this is off. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bRacingLine = true;

	/** How many of the aids there are, and so how many CountLocked can reach. */
	static constexpr int32 Count = 5;

	bool AllowsEverything() const
	{
		return bAbs && bTractionControl && bAutoGearbox && bSteeringAssist && bRacingLine;
	}

	/** How many of the five are locked. */
	int32 CountLocked() const
	{
		return (bAbs ? 0 : 1) + (bTractionControl ? 0 : 1) + (bAutoGearbox ? 0 : 1)
			+ (bSteeringAssist ? 0 : 1) + (bRacingLine ? 0 : 1);
	}

	bool operator==(const FApexAllowedAssists& Other) const
	{
		return bAbs == Other.bAbs && bTractionControl == Other.bTractionControl
			&& bAutoGearbox == Other.bAutoGearbox && bSteeringAssist == Other.bSteeringAssist
			&& bRacingLine == Other.bRacingLine;
	}
	bool operator!=(const FApexAllowedAssists& Other) const { return !(*this == Other); }
};

/** Mirrors `Weather` (data.rs). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexWeather : uint8
{
	Sunny     = 0,
	Cloudy    = 1,
	Overcast  = 2,
	LightRain = 3,
	HeavyRain = 4,
	/** Snow on a cold road. An older client shows it as sunny; a newer one as snow. */
	Snow      = 5,
};

/**
 * One step of a host-written forecast (data.rs `ForecastStep`): from AtMin
 * minutes into the session the weather is Weather. On the wire a
 * `[at_min, weather]` pair inside `SessionConditions.forecast`.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexForecastStep
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 AtMin = 0;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	EApexWeather Weather = EApexWeather::Sunny;

	bool operator==(const FApexForecastStep& Other) const { return AtMin == Other.AtMin && Weather == Other.Weather; }
	bool operator!=(const FApexForecastStep& Other) const { return !(*this == Other); }
};

/**
 * Mirrors `SessionConditions` (data.rs): the weather and the clock over a
 * session, chosen by its host on create, echoed in SessionJoined and listed in
 * every SessionSummary. The server bakes the weather's grip into the session's
 * track; the client only has to draw it. A sunny 13:00 from a server that
 * predates the field.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSessionConditions
{
	GENERATED_BODY()

	static constexpr int32 MinutesPerDay = 24 * 60;
	static constexpr int32 DefaultTimeOfDayMinutes = 13 * 60;
	static constexpr int32 WeatherCount = 6;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	EApexWeather Weather = EApexWeather::Sunny;

	/** Local time of day, minutes after midnight, 0..1439. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 TimeOfDayMinutes = DefaultTimeOfDayMinutes;

	/** The air's figures are optional: "auto" leaves them to the server,
	 * which works them out from the weather and the clock and echoes them
	 * named (server `SessionConditions::resolve`). */
	static constexpr int32 AutoAirTemp = -128;
	static constexpr int32 Auto = -1;
	static constexpr int32 MinAirTempC = -5;
	static constexpr int32 MaxAirTempC = 45;
	/** The warmest air a host may pick under snow (`Weather::SNOW_MAX_AIR_C`). */
	static constexpr int32 MaxSnowAirTempC = 2;
	static constexpr int32 MaxWindKph = 60;

	/** Air temperature, °C; AutoAirTemp when left to the weather. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 AirTempC = AutoAirTemp;

	/** Relative humidity, percent; Auto when left to the weather. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 HumidityPct = Auto;

	/** Mean wind, km/h; Auto when left to the weather. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 WindKph = Auto;

	/** Where the wind blows from, degrees in the track's frame (0 head-on
	 * down the start straight, 90 from its left); Auto when left to the
	 * server (it picks one per session). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 WindFromDeg = Auto;

	/** How the sky moves through the session (server `crate::conditions`),
	 * each Auto when unset and then left off the wire, so a create that
	 * names none keeps the bytes it always had. */
	static constexpr int32 MaxTimeScale = 60;
	static constexpr int32 MaxChangeable = 3;
	/** The rubber every car is calibrated on (`road_state::RUBBER_REFERENCE`). */
	static constexpr int32 ReferenceRubberPct = 50;

	/** How fast the day's clock runs against the session's: 0 (or Auto)
	 * holds it, 1 is real time, 24 a day in an hour. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 TimeScale = Auto;

	/** How changeable the weather is: 0 (or Auto) holds the host's sky, 1
	 * settled, 2 changeable, 3 stormy (the server's forecast). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 Changeable = Auto;

	/** Rubber on the racing line at the start, percent: 0 green, Auto the
	 * calibrated 50, 100 rubbered in. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 TrackRubberPct = Auto;

	/** The seed the server lays the forecast and the wind's direction out
	 * from (`sky_seed`, `crate::conditions::forecast`): the session's id
	 * when unset (`bHasSkySeed` false, and then left off the wire). The
	 * create screen sets one so it can show the forecast before the
	 * session exists, and roll another. */
	UPROPERTY()
	uint64 SkySeed = 0;
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	bool bHasSkySeed = false;

	static constexpr int32 DaysPerYear = 365;

	/** The date, day of the year 1..365 (`day_of_year`): how high the sun
	 * climbs and how long the day is. Auto (and left off the wire) for the
	 * model day, late May. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 DayOfYear = Auto;

	/** Solar noon on the session's clock, minutes after midnight
	 * (`solar_noon_min`): the server names it from where the circuit is
	 * and echoes it in SessionJoined; a create never sends one. Auto for
	 * 13:00 (`ApexSky::SolarNoonHours`). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	int32 SolarNoonMin = Auto;

	/** The host's own forecast (`forecast`, data.rs `HostForecast`): the
	 * server follows it instead of the one it lays out from the seed,
	 * whatever Changeable says, and echoes it as it simulates it (a winter
	 * sky's rain as snow, a summer sky's snow as light rain). Empty (and
	 * left off the wire) for none. Clamped as the server does: in time
	 * order, each step a change of weather at least ForecastMinGapMin after
	 * the last (and the start), none past ForecastHorizonMin, at most
	 * MaxForecastSteps. */
	static constexpr int32 MaxForecastSteps = 10;
	static constexpr int32 ForecastMinGapMin = 4;
	static constexpr int32 ForecastHorizonMin = 26 * 60;
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Session")
	TArray<FApexForecastStep> Forecast;

	bool HasAirTemp() const { return AirTempC != AutoAirTemp; }
	bool HasSkySeed() const { return bHasSkySeed; }
	bool HasDate() const { return DayOfYear >= 1; }
	bool HasSolarNoon() const { return SolarNoonMin >= 0; }
	bool HasForecast() const { return Forecast.Num() > 0; }
	bool HasWind() const { return WindKph >= 0; }
	bool HasWindDirection() const { return WindFromDeg >= 0; }
	bool HasTimeScale() const { return TimeScale >= 0; }
	bool HasChangeable() const { return Changeable >= 0; }
	bool HasTrackRubber() const { return TrackRubberPct >= 0; }
	/** Whether the day's clock runs. */
	bool ClockRuns() const { return TimeScale > 0; }
	/** Whether the weather follows a forecast. */
	bool WeatherChanges() const { return Changeable > 0; }

	/** "Settled", "Changeable", "Stormy" ("Fixed" for 0 and unset). */
	static FString ChangeableLabel(int32 Level)
	{
		switch (Level)
		{
		case 1:  return TEXT("Settled");
		case 2:  return TEXT("Changeable");
		case 3:  return TEXT("Stormy");
		default: return TEXT("Fixed");
		}
	}

	/** "Green track", "Rubbered track", "Track 70% rubber"; empty for the
	 * calibrated track (unset or 50). */
	static FString TrackRubberLabel(int32 Pct)
	{
		if (Pct < 0 || Pct == ReferenceRubberPct)
		{
			return FString();
		}
		if (Pct == 0)   { return TEXT("Green track"); }
		if (Pct >= 100) { return TEXT("Rubbered track"); }
		return FString::Printf(TEXT("Track %d%% rubber"), Pct);
	}

	/** "ahead", "the left", "behind", "the right" (and the diagonals):
	 * where the wind comes from, seen from the start line. */
	static FString WindFromLabel(int32 Degrees)
	{
		static const TCHAR* Names[8] = {
			TEXT("ahead"), TEXT("ahead-left"), TEXT("the left"), TEXT("behind-left"),
			TEXT("behind"), TEXT("behind-right"), TEXT("the right"), TEXT("ahead-right") };
		const int32 Wrapped = ((Degrees % 360) + 360) % 360;
		return Names[((Wrapped + 22) / 45) % 8];
	}

	/** The clock wrapped onto one day and the air held to what the sim
	 * takes, as the server does it. */
	FApexSessionConditions Clamped() const
	{
		FApexSessionConditions Out = *this;
		Out.TimeOfDayMinutes = ((TimeOfDayMinutes % MinutesPerDay) + MinutesPerDay) % MinutesPerDay;
		if (Out.HasAirTemp())
		{
			Out.AirTempC = FMath::Clamp(Out.AirTempC, MinAirTempC, Out.IsSnow() ? MaxSnowAirTempC : MaxAirTempC);
		}
		Out.HumidityPct = Out.HumidityPct < 0 ? Auto : FMath::Min(Out.HumidityPct, 100);
		Out.WindKph = Out.WindKph < 0 ? Auto : FMath::Min(Out.WindKph, MaxWindKph);
		Out.WindFromDeg = Out.WindFromDeg < 0 ? Auto : Out.WindFromDeg % 360;
		Out.TimeScale = Out.TimeScale < 0 ? Auto : FMath::Min(Out.TimeScale, MaxTimeScale);
		Out.Changeable = Out.Changeable < 0 ? Auto : FMath::Min(Out.Changeable, MaxChangeable);
		Out.TrackRubberPct = Out.TrackRubberPct < 0 ? Auto : FMath::Min(Out.TrackRubberPct, 100);
		Out.DayOfYear = Out.DayOfYear < 0 ? Auto : FMath::Clamp(Out.DayOfYear, 1, DaysPerYear);
		Out.SolarNoonMin = Out.SolarNoonMin < 0 ? Auto : Out.SolarNoonMin % MinutesPerDay;
		Out.Forecast = ClampedForecast(Weather, Forecast);
		return Out;
	}

	/** `HostForecast::from_steps`: `Steps` in time order from the start's
	 * weather, a step too soon after the last or repeating its weather
	 * dropped, cut at the horizon and the limit. */
	static TArray<FApexForecastStep> ClampedForecast(EApexWeather Start, const TArray<FApexForecastStep>& Steps)
	{
		TArray<FApexForecastStep> Sorted = Steps;
		Sorted.StableSort([](const FApexForecastStep& A, const FApexForecastStep& B) { return A.AtMin < B.AtMin; });
		TArray<FApexForecastStep> Out;
		int32 LastMin = 0;
		EApexWeather LastWeather = Start;
		for (const FApexForecastStep& Step : Sorted)
		{
			if (Out.Num() == MaxForecastSteps || Step.AtMin > ForecastHorizonMin)
			{
				break;
			}
			if (Step.AtMin < LastMin + ForecastMinGapMin || Step.Weather == LastWeather)
			{
				continue;
			}
			Out.Add(Step);
			LastMin = Step.AtMin;
			LastWeather = Step.Weather;
		}
		return Out;
	}

	bool IsWet() const { return Weather == EApexWeather::LightRain || Weather == EApexWeather::HeavyRain; }
	bool IsSnow() const { return Weather == EApexWeather::Snow; }
	bool IsDefault() const
	{
		return Weather == EApexWeather::Sunny && TimeOfDayMinutes == DefaultTimeOfDayMinutes
			&& !HasAirTemp() && HumidityPct < 0 && !HasWind() && !HasWindDirection()
			&& !HasTimeScale() && !HasChangeable() && !HasTrackRubber() && !HasSkySeed()
			&& !HasDate() && !HasSolarNoon() && !HasForecast();
	}

	/** Hours since midnight, fractional. */
	float Hours() const { return static_cast<float>(Clamped().TimeOfDayMinutes) / 60.0f; }

	/** "21:30". */
	FString ClockText() const
	{
		const int32 Minutes = Clamped().TimeOfDayMinutes;
		return FString::Printf(TEXT("%02d:%02d"), Minutes / 60, Minutes % 60);
	}

	/** "Light rain". */
	static FString WeatherLabel(EApexWeather InWeather)
	{
		switch (InWeather)
		{
		case EApexWeather::Sunny:     return TEXT("Sunny");
		case EApexWeather::Cloudy:    return TEXT("Cloudy");
		case EApexWeather::Overcast:  return TEXT("Overcast");
		case EApexWeather::LightRain: return TEXT("Light rain");
		case EApexWeather::HeavyRain: return TEXT("Heavy rain");
		case EApexWeather::Snow:      return TEXT("Snow");
		default:                      return TEXT("Sunny");
		}
	}

	/** "Light rain · 21:30 · 14°C · wind 18 km/h from the left": the air
	 * only where it is named (a session always names it; a pick may not),
	 * then a running clock ("clock x24"), a forecast ("changeable weather")
	 * and a track that is not the calibrated one ("green track"). */
	FString Describe() const
	{
		FString Out = FString::Printf(TEXT("%s · %s"), *WeatherLabel(Weather), *ClockText());
		if (HasAirTemp())
		{
			Out += FString::Printf(TEXT(" · %d°C"), AirTempC);
		}
		if (HasWind())
		{
			Out += WindKph == 0 ? FString(TEXT(" · calm"))
				: HasWindDirection()
					? FString::Printf(TEXT(" · wind %d km/h from %s"), WindKph, *WindFromLabel(WindFromDeg))
					: FString::Printf(TEXT(" · wind %d km/h"), WindKph);
		}
		if (ClockRuns())
		{
			Out += FString::Printf(TEXT(" · clock x%d"), TimeScale);
		}
		if (HasForecast())
		{
			// A written forecast is followed whatever Changeable says: name
			// its first steps instead ("forecast light rain at 20 min, sunny
			// at 45 min, 2 more").
			constexpr int32 Named = 3;
			TArray<FString> Steps;
			for (int32 Index = 0; Index < Forecast.Num() && Index < Named; ++Index)
			{
				Steps.Add(FString::Printf(TEXT("%s at %d min"), *WeatherLabel(Forecast[Index].Weather).ToLower(), Forecast[Index].AtMin));
			}
			if (Forecast.Num() > Named)
			{
				Steps.Add(FString::Printf(TEXT("%d more"), Forecast.Num() - Named));
			}
			Out += TEXT(" · forecast ") + FString::Join(Steps, TEXT(", "));
		}
		else if (WeatherChanges())
		{
			Out += FString::Printf(TEXT(" · %s weather"), *ChangeableLabel(Changeable).ToLower());
		}
		const FString Rubber = TrackRubberLabel(TrackRubberPct);
		if (!Rubber.IsEmpty())
		{
			Out += TEXT(" · ") + Rubber.ToLower();
		}
		return Out;
	}

	bool operator==(const FApexSessionConditions& Other) const
	{
		return Weather == Other.Weather && TimeOfDayMinutes == Other.TimeOfDayMinutes
			&& AirTempC == Other.AirTempC && HumidityPct == Other.HumidityPct
			&& WindKph == Other.WindKph && WindFromDeg == Other.WindFromDeg
			&& TimeScale == Other.TimeScale && Changeable == Other.Changeable
			&& TrackRubberPct == Other.TrackRubberPct
			&& bHasSkySeed == Other.bHasSkySeed && (!bHasSkySeed || SkySeed == Other.SkySeed)
			&& DayOfYear == Other.DayOfYear && SolarNoonMin == Other.SolarNoonMin
			&& Forecast == Other.Forecast;
	}
	bool operator!=(const FApexSessionConditions& Other) const { return !(*this == Other); }
};

/**
 * Where a race weekend stands (`WeekendState`, sent on join and on every
 * change): the phase, the qualifying round, whether the host may move on,
 * and who is out.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexWeekendState
{
	GENERATED_BODY()

	/** False until the session sends one (it is no weekend, or an older server). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	EApexWeekendPhase Phase = EApexWeekendPhase::Lobby;

	/** The qualifying round, 1-based; 0 outside qualifying. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 Round = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 Rounds = 0;

	/** The phase has ended and the host may move the weekend on. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bFinished = false;

	/** How many cars go through from this round; 0 in the final round and outside qualifying. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 Advancing = 0;

	/** Every car knocked out so far, in grid order. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	TArray<FApexKnockedOut> Eliminated;

	/** The phase's damage rule (practice's or qualifying's own, else the session's); Full when absent. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	EApexDamageLevel Damage = EApexDamageLevel::Full;

	/**
	 * The sky the phase runs under, for a weekend whose practice or qualifying
	 * has its own: it replaces what `SessionJoined.Conditions` said. Absent
	 * (bHasConditions false) for a weekend raced under one sky.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bHasConditions = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	FApexSessionConditions Conditions;

	const FApexKnockedOut* FindEliminated(int32 CarIndex) const
	{
		return Eliminated.FindByPredicate([CarIndex](const FApexKnockedOut& Out) { return Out.CarIndex == CarIndex; });
	}
	bool IsFinalRound() const { return Phase == EApexWeekendPhase::Qualifying && Round >= Rounds; }
};

/**
 * A race weekend, as CreateSession's `weekend` and SessionJoined's `Weekend`
 * carry it (network.rs `WeekendFormat`): timed practice (0 for none), then
 * one to three timed qualifying rounds run as a knockout, then the race over
 * the session's own laps or time. The host moves it on (AdvanceWeekend).
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexWeekendFormat
{
	GENERATED_BODY()

	static constexpr int32 MaxRounds = 3;

	/** Practice's length, seconds (ApexQualifyingLength bounds); 0 for no practice. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	int32 PracticeSeconds = 0;

	/** Each qualifying round's length, seconds; empty when the session is no weekend. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	TArray<int32> QualifyingSeconds;

	/**
	 * Practice's and the qualifying day's own sky (`practice_conditions`,
	 * `qualifying_conditions`): a day not named runs under the session's
	 * own conditions, the race day's.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bHasPracticeConditions = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	FApexSessionConditions PracticeConditions;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bHasQualifyingConditions = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	FApexSessionConditions QualifyingConditions;

	/**
	 * Practice's and every qualifying round's own damage rule
	 * (`practice_damage`, `qualifying_damage`): a phase not named, and the
	 * race, run under the session's own Damage.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bHasPracticeDamage = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	EApexDamageLevel PracticeDamage = EApexDamageLevel::Full;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	bool bHasQualifyingDamage = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Weekend")
	EApexDamageLevel QualifyingDamage = EApexDamageLevel::Full;

	bool IsWeekend() const { return QualifyingSeconds.Num() > 0; }
	int32 Rounds() const { return QualifyingSeconds.Num(); }

	/** As the server keeps it: lengths in bounds, at most MaxRounds rounds, each day's sky clamped. */
	FApexWeekendFormat Clamped() const
	{
		FApexWeekendFormat Out;
		Out.bHasPracticeConditions = bHasPracticeConditions;
		Out.PracticeConditions = bHasPracticeConditions ? PracticeConditions.Clamped() : FApexSessionConditions();
		Out.bHasQualifyingConditions = bHasQualifyingConditions;
		Out.QualifyingConditions = bHasQualifyingConditions ? QualifyingConditions.Clamped() : FApexSessionConditions();
		Out.bHasPracticeDamage = bHasPracticeDamage;
		Out.PracticeDamage = bHasPracticeDamage ? PracticeDamage : EApexDamageLevel::Full;
		Out.bHasQualifyingDamage = bHasQualifyingDamage;
		Out.QualifyingDamage = bHasQualifyingDamage ? QualifyingDamage : EApexDamageLevel::Full;
		Out.PracticeSeconds = ApexQualifyingLength::Clamp(PracticeSeconds);
		for (int32 Index = 0; Index < FMath::Min(QualifyingSeconds.Num(), MaxRounds); ++Index)
		{
			Out.QualifyingSeconds.Add(FMath::Max(ApexQualifyingLength::MinSeconds, ApexQualifyingLength::Clamp(QualifyingSeconds[Index])));
		}
		return Out;
	}
};

/** Mirrors `GameMode` (data.rs:906). Serialize_repr => a plain u8 on the wire. */
UENUM(BlueprintType)
enum class EApexGameMode : uint8
{
	Lobby         = 0,
	Sandbox       = 1,
	Countdown     = 2,
	DemoLap       = 3,
	FreePractice  = 4,
	Replay        = 5,
	Qualification = 6,
	Race          = 7,
	/**
	 * Time attack: every driver starts in the garage (tuning, not simulated),
	 * goes out onto a run-up before the line for flying laps and can come
	 * back in at any time (HotlapRelocate). Cars in the garage are neither
	 * ticked nor collided with, so several drivers can hotlap side by side.
	 */
	Hotlap        = 8,
	/**
	 * A race weekend's timed practice: run from the garage as qualifying is
	 * (out from the pit exit on an outlap), timed, but nothing it sets
	 * counts for the grid.
	 */
	Practice      = 9,
	/**
	 * The race's grid before its countdown: the cars stand on their slots
	 * with no clock while each driver picks setup, tyres and livery and says
	 * they are ready (SetReady). Once every connected driver is, the server
	 * counts into the race; the host's StartCountdown starts it at once.
	 */
	Grid          = 10,
};

/**
 * Hotlap, qualifying and a weekend's practice run on the same garage rules:
 * every human starts in the garage, the car is frozen until it goes out, and
 * the setup is tuned there.
 */
inline bool ApexIsGarageMode(EApexGameMode Mode)
{
	return Mode == EApexGameMode::Hotlap || Mode == EApexGameMode::Qualification || Mode == EApexGameMode::Practice;
}

/** The timed garage sessions, which go out from the pit exit on an outlap: qualifying and a weekend's practice. */
inline bool ApexIsOutlapMode(EApexGameMode Mode)
{
	return Mode == EApexGameMode::Qualification || Mode == EApexGameMode::Practice;
}


/** Mirrors `HotlapDestination` (data.rs): where a hotlap driver asks to be put. */
UENUM(BlueprintType)
enum class EApexHotlapDestination : uint8
{
	/** Back to the garage: parked, frozen, out of everyone's way. */
	Garage = 0,
	/** Onto the track, on the run-up before the start line. */
	Track  = 1,
};

/** Mirrors `RecoverDestination` (recovery.rs): where a stuck driver asks for their car to be put. */
UENUM(BlueprintType)
enum class EApexRecoverDestination : uint8
{
	/** Back onto the track where the car is, held for the time cost. */
	Track = 0,
	/** To the car's pit box, serviced and driven out (the garage in a hotlap or qualifying). */
	Pits  = 1,
};

/** Client-side connection lifecycle. Not a protocol type. */
UENUM(BlueprintType)
enum class EApexConnectionState : uint8
{
	Disconnected,
	Connecting,
	Authenticating,
	Authenticated,
	/** Auth was rejected or the network thread could not start; nothing retries. */
	Failed,
	/**
	 * The server could not be reached or dropped the connection, and the
	 * subsystem is retrying on a backoff. Cleared by a successful connect,
	 * an AuthFailure (-> Failed) or an explicit Disconnect().
	 */
	Reconnecting,
};

/**
 * IDs are kept as FString, deliberately not FGuid: Rust emits lowercase
 * hyphenated UUIDs, while FGuid::ToString() defaults to uppercase without
 * hyphens. Round-tripping through FGuid would silently corrupt every ID we
 * send back. Compare with ESearchCase::IgnoreCase throughout.
 */

/** `LobbyPlayer` (network.rs:242) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLobbyPlayer
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Name;

	/** Empty when the player has not picked a car (nil on the wire). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString SelectedCar;

	/** Empty when the player is not in a session (nil on the wire). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString InSession;

	bool HasSelectedCar() const { return !SelectedCar.IsEmpty(); }
};

/** `SessionSummary` (network.rs:266) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSessionSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString TrackName;

	/** Track file relative to the content folder, e.g. "tracks/default/Armadillos/Armadillos.yaml". */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString TrackFile;

	/** The track config the session runs on; joins the track summaries and DT_TrackCatalog. Empty from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString TrackId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString HostName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	EApexSessionKind SessionKind = EApexSessionKind::Multiplayer;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 PlayerCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 MaxPlayers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	EApexSessionState State = EApexSessionState::Lobby;

	/** The session's weather and clock; a sunny afternoon from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FApexSessionConditions Conditions;

	/** How much damage the cars take here; Full (the default) from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	EApexDamageLevel Damage = EApexDamageLevel::Full;

	/** The race distance in laps; 0 from an older server, and in a timed race. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 LapLimit = 0;

	/** A timed race's length in seconds (ApexRaceLength); 0 for a race over laps. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 RaceSeconds = 0;

	/**
	 * A race weekend between phases, waiting for its host to advance it:
	 * listed Finished, it still takes drivers. False from an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	bool bWeekendWaiting = false;

	/** The race's mandatory stops (ApexMandatoryStops); 0 for none or from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 MandatoryStops = 0;

	/** The race must be run on two dry compounds. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	bool bCompoundChange = false;

	/** A race is being driven: there is something to watch. */
	bool IsWatchable() const { return State == EApexSessionState::Countdown || State == EApexSessionState::Racing; }

	/** Over for good: Finished, and not a weekend waiting for its next phase. */
	bool IsOver() const { return State == EApexSessionState::Finished && !bWeekendWaiting; }

	/**
	 * Room for another driver in a session not yet over. A session under way
	 * takes one too (a hotlap, a practice), and so does a weekend between
	 * phases; the server seats them.
	 */
	bool IsJoinable() const { return !IsOver() && PlayerCount < MaxPlayers; }
};

/** `CarConfigSummary` (network.rs:285) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarConfigSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Name;

	/**
	 * Informational only — DO NOT resolve this to a file. The server builds it
	 * as `res://content/cars/{uuid}/{model}` (broadcast.rs:150) but the folders
	 * on disk are named `fugazzi-sf26` etc, so the path never exists. Meshes
	 * are resolved through DT_CarCatalog keyed by Id instead.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString ModelPath;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float MassKg = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float MaxEngineForceN = 0.0f;

	/**
	 * Checksum of the car.toml the server loaded (content_crc.rs); compared
	 * with the DT_CarCatalog row's SourceCrc when the car is used. 0 from a
	 * server that predates it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int64 ContentCrc = 0;
};

/** `TrackConfigSummary` (network.rs:326) — PascalCase keys, but see FApexTrackPoint. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTrackConfigSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	FString Name;

	/**
	 * Every 10th centerline point (broadcast.rs:162). Only populated when the
	 * cvar `apexsim.net.ParseCenterline` is on; the menu shell has no use for
	 * it and parsing ~14k points twice a minute is not free.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FVector2D> Centerline;

	/**
	 * Checksum of the track file the server loaded (content_crc.rs); compared
	 * with the DT_TrackCatalog row's SourceCrc when the level is streamed. 0
	 * from a server that predates it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int64 ContentCrc = 0;

	/**
	 * Where the circuit is (`LatitudeDeg`, `LongitudeDeg`), when the server's
	 * track names a longitude: what the session's solar noon is worked out
	 * from (ApexSky::SolarNoonMinutes). Not sent for a track that does not
	 * say, nor by an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	bool bHasSite = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float LatitudeDeg = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	float LongitudeDeg = 0.0f;
};

/** `LobbyStateData` (network.rs:174) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLobbyState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexLobbyPlayer> PlayersInLobby;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexSessionSummary> AvailableSessions;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexCarConfigSummary> CarConfigs;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	TArray<FApexTrackConfigSummary> TrackConfigs;

	/**
	 * The server plays showcases (rendered races a client in the menu watches
	 * instead of asking for a demo session); false from a server that
	 * predates them.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	bool bShowcaseAvailable = false;

	/**
	 * A live session can be watched as a spectator stream (`SpectateSession`):
	 * the lap timing and pit stops so far come with it, which the racers'
	 * telemetry never carried. False from a server that predates it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	bool bLiveStream = false;

	/**
	 * The port of the server's health endpoint when it hands out its
	 * recordings of finished sessions (`/recordings`, on the address
	 * connected to); 0 when it does not.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Lobby")
	int32 RecordingsPort = 0;
};

/** What a spectator stream is of (`SpectatorKind`, network.rs). */
UENUM(BlueprintType)
enum class EApexSpectatorKind : uint8
{
	/** A rendered race played from a file. */
	Showcase = 0,
	/** A session being raced now. */
	Live = 1,
};

/**
 * One driver's line of a stored qualifying result (`QualifyingEntryData`,
 * network.rs): who, in what car, and the best legal lap.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexQualifyingEntry
{
	GENERATED_BODY()

	/** The name the server grids by: a human's, or the AI's own. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	FString CarConfigId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	int32 LapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	bool bIsAi = false;
};

/**
 * One line of a leaderboard (`LeaderboardEntryData`): a driver's best lap in
 * a car, answering RequestLeaderboard.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLeaderboardEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Leaderboard")
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Leaderboard")
	FString CarConfigId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Leaderboard")
	int32 LapTimeMs = 0;

	/** Each sector's time, ms. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Leaderboard")
	TArray<int32> SplitsMs;

	/** RFC 3339. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Leaderboard")
	FString RecordedAt;
};

/**
 * A qualifying session's classification as the server stores it
 * (`QualifyingResultData`): the drivers fastest first, answering
 * RequestQualifyingResults for a track.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexQualifyingResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	FString Id;

	/** The class the session ran in as car.toml spells it (`GT3`); shown through DisplayClass. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	FString Class;

	/** RFC 3339, `2026-10-06T12:00:00Z`. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	FString RecordedAt;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Qualifying")
	TArray<FApexQualifyingEntry> Entries;
};

/** One showcase channel as `Showcases` lists it (`ShowcaseSummary`, network.rs) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexShowcaseSummary
{
	GENERATED_BODY()

	/** What `SpectateShowcase` takes. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	FString TrackId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	FString TrackName;

	/** The field's class as car.toml spells it (`GT3`, `F1`). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	FString Class;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	FApexSessionConditions Conditions;

	/** Seconds of race before it starts over. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	float DurationS = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	int32 Cars = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Showcase")
	int32 Viewers = 0;
};

/** `AuthSuccessData` (network.rs:131) — PascalCase keys. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexAuthSuccess
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	int64 ServerVersion = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	int32 ProtocolVersion = 0;

	/** Names this connection in the UDP handshake. Unused by the menu shell. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	FString UdpToken;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	int32 UdpPort = 0;

	/**
	 * Secret under which every datagram we send is sealed
	 * (ApexProtocol::SealUdpDatagram). Hex, used as raw bytes; it only ever
	 * travels here, over TLS. Empty from a server from before sealing, which
	 * takes no sealed datagram either: UDP is then not started.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	FString UdpKey;

	/** Secret to present in a later `Authenticate` to be this player again
	 *  (and take back a seat held in a session); empty from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Auth")
	FString ResumeToken;
};

/**
 * One car in a `SessionRoster` (network.rs:457) — PascalCase keys, TCP.
 *
 * Compact telemetry identifies cars by a session-scoped index rather than a
 * UUID, and this is the only thing that maps that index back to a player.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexRosterEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CarIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsAi = false;

	/** The car this entry drives (the catalog's key); empty from an older server. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString CarConfigId;

	/**
	 * The livery it wears: 0 the car as authored, 1.. the catalog row's
	 * `Liveries` (the car.toml's `[[livery]]` tables). 0 from an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Livery = 0;

	/** The driver lost their connection; the server drives the car until they rejoin. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bAway = false;

	/** The car's starting place, 1 = pole; 0 when the session has not dealt one (or an older server). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 GridSlot = 0;

	/** On the race's grid (EApexGameMode::Grid): the driver said they are ready. False everywhere else. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bReady = false;

	/**
	 * Did not qualify: the session's qualifying cut-off (the 107% rule) left
	 * the car out of the race, parked and towed. Not a retirement. False
	 * from an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bDnq = false;

	/**
	 * Took the flag owing a stop or its second compound under the session's
	 * pit rule (ApexMandatoryStops): classified behind every car that met
	 * it. False from an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bMissedStops = false;

	/**
	 * Qualifying under a lap allowance: the timed laps this car has ended in
	 * the session or round, as the server counts them (`QualifyingLaps`; 0,
	 * left off the wire, outside one). UApexNetSubsystem::GetQualifyingLapsLeft.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 QualifyingLaps = 0;
};

/** `SessionRosterData` (network.rs:470) — PascalCase keys, TCP. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSessionRoster
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FApexRosterEntry> Entries;
};

/** What the driver does at a point of the racing line. The wire value is a u8. */
UENUM(BlueprintType)
enum class EApexLinePhase : uint8
{
	/** Flat out, or accelerating as hard as the car can. */
	Throttle = 0,
	/** At the grip limit through a corner, or lifting for a kink. */
	Partial  = 1,
	Brake    = 2,
};

/**
 * `RacingLineData` (network.rs) — PascalCase keys, TCP, sent once right after
 * SessionJoined.
 *
 * The line the joining player's car should take: a closed loop of evenly
 * spaced points, each tagged with what to do there. The server builds it from
 * the track's raceline and the car's grip, power and brakes, so the braking
 * points are that car's. On the wire the positions are parallel X/Y/Z arrays;
 * they are zipped into points here.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexRacingLineData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString SessionId;

	/** Distance between consecutive points, metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float SpacingM = 0.0f;

	/** Server-frame positions, metres. The last point joins back to the first. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FVector> Points;

	/** One per point. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<EApexLinePhase> Phases;

	bool IsValid() const { return Points.Num() >= 3 && Phases.Num() == Points.Num(); }
};

/**
 * `TrackSectorsData` (network.rs) — where the session's sector lines are.
 *
 * Sent once, with the racing line. Sector 1 always begins at the start line,
 * so `BoundariesM` holds the other two; a car's sector is read straight off
 * the station telemetry already carries.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTrackSectors
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString SessionId;

	/** Lap length, metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TrackLengthM = 0.0f;

	/** Station of each boundary past the line, ascending, metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<float> BoundariesM;

	bool IsValid() const { return TrackLengthM > 1.0f && BoundariesM.Num() > 0; }

	/** Which sector (0-based) a car standing at `StationM` is driving. */
	int32 SectorAt(float StationM) const
	{
		int32 Sector = 0;
		for (int32 Index = 0; Index < BoundariesM.Num(); ++Index)
		{
			if (StationM >= BoundariesM[Index])
			{
				Sector = Index + 1;
			}
		}
		return Sector;
	}

	/** Sectors in a lap: one more than the boundaries past the line. */
	int32 SectorCount() const { return BoundariesM.Num() + 1; }
};

/** One corner (or chicane, or esses) of the lap: `CornerData` (network.rs). */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTrackCorner
{
	GENERATED_BODY()

	/** From 1, in lap order from the start line. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Number = 0;

	/** The dossier's display name; empty when it has none ("Turn N"). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString Name;

	/** Stations from the line, each in [0, lap): the corner runs forward from the entry (past the line, if need be) to the exit. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float EntryM = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float ApexM = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float ExitM = 0.0f;

	/** The main turn goes left. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bLeft = false;

	/** The name to show: the dossier's, else "Turn N". */
	FString Label() const
	{
		return Name.IsEmpty() ? FString::Printf(TEXT("Turn %d"), Number) : Name;
	}
};

/**
 * `TrackCornersData` (network.rs) — the track's corners in lap order.
 *
 * Sent to the watcher of a hotlap (`SessionKind::HotlapWatch`) with the sector
 * lines, so the HUD can say which corner the car is in or coming to.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTrackCorners
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString SessionId;

	/** Lap length, metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TrackLengthM = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FApexTrackCorner> Corners;

	bool IsValid() const { return TrackLengthM > 1.0f && Corners.Num() > 0; }

	/** Metres from `FromM` forward along the lap to `ToM`, in [0, lap). */
	float Ahead(float FromM, float ToM) const
	{
		return TrackLengthM > 0.0f ? FMath::Fmod(FMath::Fmod(ToM - FromM, TrackLengthM) + TrackLengthM, TrackLengthM) : 0.0f;
	}

	/** Whether the car at `StationM` is between a corner's entry and exit. */
	bool IsInside(const FApexTrackCorner& Corner, float StationM) const
	{
		return Ahead(Corner.EntryM, StationM) <= Ahead(Corner.EntryM, Corner.ExitM);
	}

	/**
	 * The corner the car at `StationM` is in, else the next one it comes to
	 * (index into Corners, INDEX_NONE without any). `OutDistanceM` is 0 inside
	 * a corner, else the metres to its entry; `bOutInside` says which.
	 */
	int32 CornerAt(float StationM, float& OutDistanceM, bool& bOutInside) const
	{
		int32 Best = INDEX_NONE;
		OutDistanceM = 0.0f;
		bOutInside = false;
		for (int32 Index = 0; Index < Corners.Num(); ++Index)
		{
			if (IsInside(Corners[Index], StationM))
			{
				OutDistanceM = 0.0f;
				bOutInside = true;
				return Index;
			}
			const float Distance = Ahead(StationM, Corners[Index].EntryM);
			if (Best == INDEX_NONE || Distance < OutDistanceM)
			{
				Best = Index;
				OutDistanceM = Distance;
			}
		}
		return Best;
	}
};

/**
 * `RoadStateData` (network.rs) — a slice of the session's road state:
 * rubber, marbles, a dry line and water per cell across the road, and the
 * debris on it. The lap is cut into cells of `CellM`, each across the road
 * into `Bins` bins reaching `HalfSpanM` either side of the centerline.
 * `Rows` holds whole cells from `FirstCell` (wrapping the lap), RowBytes()
 * each: the cell's water depth (100 = heavy rain on the flat, over 100 a
 * puddle), then per bin rubber, marbles and dry, each a byte over 0..1.
 *
 * Sent round-robin while racing and whole on join (also fed from a stream's
 * `Road` record for showcases and replays); the race director reads it into
 * the road-state texture the road material samples by its (station, lateral)
 * UV. Not a UPROPERTY struct: the rows are raw bytes for a texture upload.
 */
struct APEXSIMNET_API FApexRoadState
{
	FString SessionId;
	float LapM = 0.0f;
	float CellM = 10.0f;
	int32 Bins = 32;
	float HalfSpanM = 16.0f;
	int32 FirstCell = 0;
	TArray<uint8> Rows;
	/**
	 * The snow of the same cells (`SnowRows`): SnowRowBytes() each, the verge's
	 * cover then per bin the cover and the ice, bytes over 0..1. Empty in a
	 * session without snow, and from a server that predates it.
	 */
	TArray<uint8> SnowRows;
	/**
	 * The water standing on each bin of the same cells (`WaterRows`): Bins
	 * bytes each, in the rows' depth units (100 heavy rain on the flat), so
	 * a puddle lies where it is across the road. Empty when no puddle
	 * stands, and from a server that predates it: each row's depth then
	 * covers its cell's width.
	 */
	TArray<uint8> WaterRows;
	/**
	 * The pit lane's snow cover (`LaneSnow`), a byte over 0..1 a 10 m cell of
	 * the lane's own station, the whole lane; empty without snow or a lane.
	 */
	TArray<uint8> LaneSnow;
	/** Debris pieces on the road now, server frame, metres. */
	TArray<FVector2D> Debris;
	/**
	 * Where each cell in Rows lies: X, Y the centerline at its middle and
	 * Z, W the unit vector to its left (server frame, metres). A bin's
	 * lateral is measured from this point along this vector, positive right,
	 * as the server's `RoadState::lateral_of` does.
	 */
	TArray<FVector4f> Geometry;

	/** Decode a geometry bin (four little-endian f32 per cell) into Geometry. */
	void SetGeometryBytes(TArrayView<const uint8> Bytes)
	{
		const int32 Cells = Bytes.Num() / 16;
		Geometry.SetNumUninitialized(Cells);
		for (int32 i = 0; i < Cells; ++i)
		{
			float V[4];
			FMemory::Memcpy(V, Bytes.GetData() + i * 16, 16);
			Geometry[i] = FVector4f(V[0], V[1], V[2], V[3]);
		}
	}

	/** Bytes of one cell of SnowRows: the verge, then cover and ice per bin. */
	int32 SnowRowBytes() const { return 1 + 2 * FMath::Max(Bins, 0); }
	bool HasSnow() const { return SnowRows.Num() >= SnowRowBytes() && SnowRowBytes() > 1; }
	/** Whether WaterRows holds every cell of Rows. */
	bool HasWaterRows() const { return Bins > 0 && WaterRows.Num() >= NumCells() * Bins && NumCells() > 0; }
	/** Bytes of one cell: its depth, then rubber/marbles/dry per bin. */
	int32 RowBytes() const { return 1 + 3 * FMath::Max(Bins, 0); }
	/** Whole cells in Rows. */
	int32 NumCells() const { const int32 B = RowBytes(); return B > 0 ? Rows.Num() / B : 0; }
	/** Cells round the whole lap. */
	int32 LapCells() const { return CellM > 0.0f ? FMath::Max(1, FMath::CeilToInt(LapM / CellM)) : 0; }
	bool IsValid() const { return LapM > 1.0f && CellM > 0.0f && Bins > 0 && NumCells() > 0; }
};

/**
 * `LapTimingData` (network.rs) — one car crossed a timing line.
 *
 * Timed on the server at the full tick rate; a client timing splits off 60 Hz
 * telemetry snapshots would be tens of milliseconds out.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLapTiming
{
	GENERATED_BODY()

	/** Index into the current session roster. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CarIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Lap = 0;

	/** The sector just completed, 0-based. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Sector = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SectorTimeMs = 0;

	/** The lap time when this sector closed the lap, 0 otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsLapEnd = false;

	/** The lap was inside track limits. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPersonalBestLap = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bSessionBestLap = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPersonalBestSector = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bSessionBestSector = false;
};

/**
 * `PitServiceData` (network.rs) — a car has stopped at its box and the crew
 * has started work. Sent once per stop, reliably, to every human in the
 * session.
 *
 * The parts run in order: tyres, then fuel, then repairs; a part of 0 s is
 * not done. Telemetry's `ServiceSecondsLeft` counts `TotalS` down while
 * `bPitServicing` is set, so the time into the stop is TotalS less it.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexPitService
{
	GENERATED_BODY()

	/** Index into the current session roster. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CarIndex = 0;

	/** The box the car stopped at, 0-based. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 PitBox = 0;

	/** Seconds for the tyre change; 0 when the tyres stay on. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TyresS = 0.0f;

	/** The set going on: 0 soft, 1 medium, 2 hard. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Compound = 1;

	/** Seconds of refuelling, and the litres it puts in. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float FuelS = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float FuelL = 0.0f;

	/** Seconds of repairs, and the damage they take off (percent, summed over the zones). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float RepairS = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float RepairPct = 0.0f;

	/** The whole stop, seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TotalS = 0.0f;
};

/**
 * `pit::PitPlan` (pit.rs), sent as `SetPitPlan` — the driver's orders for
 * their next stop, at any time in a session; the server keeps the last one.
 * Every field at its default is the crew's own choice (the setup's next
 * compound, fuel to need, repairs, pads past their change point), and is
 * left off the wire.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexPitPlan
{
	GENERATED_BODY()

	/** The tyres stay on. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bKeepTyres = false;

	/** The set going on, an index into the car's compounds; -1 for the setup's choice. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 Compound = -1;

	/** Litres to put in (0 for none); negative for what the rest of the run needs. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	float FuelL = -1.0f;

	/** The damage is left as it is. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bNoRepair = false;

	/** New brake pads: -1 when worn past the change point (the crew's call), 0 never, 1 always. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 Pads = -1;

	/** The set new pads are when bChoosePadSet: -1 endurance, 0 standard, 1 sprint
	 * (the setup's brake_pads knob follows it); otherwise the setup's set. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bChoosePadSet = false;
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 PadSet = 0;

	/** Box this lap: comes back as FApexCarTelemetry::bPitRequested until the stop. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bBoxThisLap = false;
};

/**
 * `PitBoxData` (network.rs) — the local driver's own pit box, sent once the
 * server deals it and again after a rejoin. In the server's frame: metres,
 * yaw in radians, +Y left (ApexRace::ServerToUnrealPosition converts).
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexPitBox
{
	GENERATED_BODY()

	/** Whether the server has said; nothing else here means anything until it has. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bValid = false;

	/** The box, 0-based from the lane's entry end. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 PitBox = 0;

	/** The stop spot: where the car's middle stands for the crew, facing down the lane. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FVector Position = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float YawRad = 0.0f;

	/** The spot's distance off the lane's middle, m, positive to the left of the yaw (the garage side). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float LateralM = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float LaneWidthM = 0.0f;

	/** The car's middle must stop within this of the spot, m. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float RadiusM = 0.0f;

	/** The driver drives the pit lane themselves (the pit autopilot is off): the box is marked. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bManual = false;
};

/**
 * `LapRecordData` (network.rs) — the driver's stored best on this track in
 * this car, sent on joining and again whenever they beat it.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexLapRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString TrackId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString CarConfigId;

	/** Their best legal lap ever here in this car; 0 when they have none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> SplitsMs;

	/** This message announces a record just beaten. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsNew = false;

	/** The fastest lap anyone has set here in this car; 0 when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 TrackRecordMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString TrackRecordHolder;

	/** The record lap has a stored trace, so a ghost can be driven from it. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bHasGhost = false;
};

/** One moment of a recorded lap, in the server frame (metres, radians). */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexGhostSample
{
	GENERATED_BODY()

	/** Milliseconds since the lap started. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 TimeMs = 0;

	/** Server frame: metres, +X along the track, +Y left. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float YawRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float PitchRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float RollRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float SpeedMps = 0.0f;

	/** -1..1, positive to the left, as the server holds it. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Steering = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Gear = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float EngineRpm = 0.0f;
};

/**
 * `GhostLapData` (network.rs) — the trace of the driver's record lap on the
 * session's track in their car, asked for with RequestGhost. The wire
 * carries it as struct-of-arrays; here it is one sample per moment, which
 * is what a puppet car reads.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexGhostLap
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString TrackId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FString CarConfigId;

	/** The lap the trace was recorded on; 0 when there is none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float SampleHz = 20.0f;

	/** Ascending in TimeMs, from the line to the line. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FApexGhostSample> Samples;

	bool IsValid() const { return LapTimeMs > 0 && Samples.Num() >= 2; }

	/**
	 * The lap's pose at `TimeMs`, blended between the samples either side
	 * (the yaw the short way round). Before the first sample it is the
	 * first; past the last it is the last, and the call returns false so a
	 * ghost that has finished its lap can be hidden.
	 */
	bool SampleAt(float TimeMs, FApexGhostSample& Out) const;
};

/**
 * One car's timing sheet, built from the `LapTiming` messages it has sent.
 *
 * The server is the stopwatch; this is only the tally, so the HUD and the
 * standings can read a car's sectors without recomputing anything.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarTiming
{
	GENERATED_BODY()

	/** Splits of the lap in progress; 0 where the sector is not done yet. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> CurrentSplitsMs;

	/** Splits of the last completed lap. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> LastSplitsMs;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LastLapMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bLastLapValid = false;

	/** Best legal lap this session, 0 when the car has none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 BestLapMs = 0;

	/** The splits that best lap was made of. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> BestLapSplitsMs;

	/** Best time in each sector this session, whatever lap it came from. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> BestSplitsMs;

	/** The sector the car was last seen to complete, -1 before the first. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LastSector = -1;

	/**
	 * The three best sectors added up — the lap the driver has already shown
	 * they can do. 0 until every sector has a time.
	 */
	int32 OptimalLapMs() const
	{
		int32 Total = 0;
		for (int32 Split : BestSplitsMs)
		{
			if (Split <= 0)
			{
				return 0;
			}
			Total += Split;
		}
		return BestSplitsMs.Num() > 0 ? Total : 0;
	}
};

/**
 * Every car's timing sheet for the session, plus the session bests.
 *
 * Fed one `LapTiming` message at a time; nothing here is derived from
 * telemetry, so a dropped frame costs nothing.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTimingBoard
{
	GENERATED_BODY()

	/** By car index, as the roster numbers them. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TMap<int32, FApexCarTiming> Cars;

	/** The fastest legal lap anyone has set, and who set it. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SessionBestLapMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SessionBestLapCarIndex = -1;

	/** The fastest each sector has been driven by anyone. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<int32> SessionBestSplitsMs;

	/** How many sectors a lap has here; 0 until the track sectors arrive. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 SectorCount = 0;

	const FApexCarTiming* Find(int32 CarIndex) const { return Cars.Find(CarIndex); }

	void Reset(int32 InSectorCount = 0)
	{
		Cars.Reset();
		SessionBestLapMs = 0;
		SessionBestLapCarIndex = -1;
		SectorCount = InSectorCount;
		SessionBestSplitsMs.Init(0, FMath::Max(0, InSectorCount));
	}

	/** Take one crossing. Safe to call before the sector count is known. */
	void Apply(const FApexLapTiming& Timing)
	{
		if (Timing.Sector < 0)
		{
			return;
		}
		if (SectorCount <= Timing.Sector)
		{
			SectorCount = Timing.Sector + 1;
		}
		auto Fit = [this](TArray<int32>& Splits)
		{
			if (Splits.Num() < SectorCount)
			{
				Splits.SetNumZeroed(SectorCount);
			}
		};
		Fit(SessionBestSplitsMs);

		FApexCarTiming& Car = Cars.FindOrAdd(Timing.CarIndex);
		Fit(Car.CurrentSplitsMs);
		Fit(Car.LastSplitsMs);
		Fit(Car.BestSplitsMs);
		Fit(Car.BestLapSplitsMs);

		Car.CurrentSplitsMs[Timing.Sector] = Timing.SectorTimeMs;
		Car.LastSector = Timing.Sector;

		// A sector best is only a best when the lap it is on is legal, which
		// is the server's call: it sets the flag, the client only files it.
		if (Timing.bPersonalBestSector)
		{
			Car.BestSplitsMs[Timing.Sector] = Timing.SectorTimeMs;
		}
		if (Timing.bSessionBestSector)
		{
			SessionBestSplitsMs[Timing.Sector] = Timing.SectorTimeMs;
		}

		if (!Timing.bIsLapEnd)
		{
			return;
		}
		Car.LastSplitsMs = Car.CurrentSplitsMs;
		Car.LastLapMs = Timing.LapTimeMs;
		Car.bLastLapValid = Timing.bValid;
		if (Timing.bPersonalBestLap)
		{
			Car.BestLapMs = Timing.LapTimeMs;
			Car.BestLapSplitsMs = Car.CurrentSplitsMs;
		}
		if (Timing.bSessionBestLap)
		{
			SessionBestLapMs = Timing.LapTimeMs;
			SessionBestLapCarIndex = Timing.CarIndex;
		}
		Car.CurrentSplitsMs.Init(0, SectorCount);
	}
};

/**
 * One car from `CompactCarState` (network.rs:388).
 *
 * Only the fields the client actually uses are kept; the rest are still read
 * positionally because skipping is not optional in a positional encoding —
 * every field must be consumed in order to stay aligned.
 *
 * Coordinates are the server's: right-handed, metres, +X along the track, +Y
 * left, angles counter-clockwise from +X.
 */
enum class EApexContactSurface : uint8
{
	Road = 0,
	/** Within the curb band past the road edge. */
	Curb = 1,
	/** Past the curbs: grass, gravel, the verge. */
	Off  = 2,
};

USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexCarTelemetry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CarIndex = 0;

	/** Server-space position, in metres. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float YawRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float PitchRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float RollRad = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float SpeedMps = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Throttle = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Brake = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float Steering = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Gear = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float EngineRpm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CurrentLap = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TrackProgress = 0.0f;

	/**
	 * Classified position once the car has completed the race distance, 1 for
	 * the winner; 0 while it is still racing (the server sends nil). Set in
	 * crossing order and never changed afterwards.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 FinishPosition = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CurrentLapTimeMs = 0;

	/** The car's last completed lap, 0 when it has not finished one. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 LastLapTimeMs = 0;

	/** Its best lap *inside track limits* this session, 0 when it has none. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 BestLapTimeMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsOnTrack = true;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bIsColliding = false;

	/** The lap in progress has been struck for leaving the track. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bLapInvalid = false;

	/** The lap this car just completed was struck. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bLastLapInvalid = false;

	/**
	 * The car is parked in the garage of a hotlap session: not simulated,
	 * not to be drawn on the track. `lap_flags` bit 2.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bInGarage = false;

	/** The car may open its DRS flap where it is. `lap_flags` bit 3. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bDrsAllowed = false;

	/** The DRS flap is open. `lap_flags` bit 4. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bDrsOpen = false;

	/**
	 * The headlights are on: the driver's switch, or the session's sky when
	 * the switch is untouched (server `headlights.rs`). `lap_flags` bit 5.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bHeadlights = false;

	/** The driver is flashing the headlights. `lap_flags` bit 6. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bHeadlightFlash = false;

	/**
	 * Litres in the tank, to a tenth (`fuel_dl`); negative when the server
	 * does not send it. Zero is a dry tank: the engine makes no power.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float FuelLiters = -1.0f;

	/**
	 * Each tyre's tread temperature, °C, FL FR RL RR (`tyre_c`); negative
	 * when unknown (an older server, or a car not yet on its tyres). Plain
	 * members: Blueprint cannot take a fixed array.
	 */
	float TyreTempC[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

	/** Each tyre's running pressure, kPa, FL FR RL RR (`tyre_kpa`); negative when unknown. */
	float TyrePressureKpa[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

	/** Whether the server sent tyre temperatures for this car. */
	bool HasTyres() const { return TyreTempC[0] >= 0.0f; }

	/**
	 * The tow the car is in: the share of its drag the wake of the cars
	 * ahead saves, 0-1 (`tow_pct`, server `slipstream.rs`); negative when
	 * the server does not send it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float TowShare = -1.0f;

	/** Each tyre's wear, percent, FL FR RL RR (`tyre_wear`); negative when the
	 * server does not send it. */
	float TyreWearPct[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

	/** The compound on the car: 0 soft, 1 medium, 2 hard (server
	 * `tyre_thermal::COMPOUNDS`); -1 unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 Compound = -1;

	/** In the pit lane; between its lines, on the limiter; stopped at the box
	 * being serviced, with the seconds left (`pit_flags`, `service_ds`). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bInPitLane = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPitLimiter = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPitServicing = false;
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float ServiceSecondsLeft = 0.0f;

	/** The server is driving the car along the pit route (`pit_flags` bit 3):
	 * a human's car is taken over from the lane's mouth to its end, and the
	 * player's input is ignored meanwhile. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPitAutopilot = false;
	/** The pit exit light is red (bit 4; the same on every car). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPitExitClosed = false;
	/** The car is waiting at the red pit exit light (bit 5). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPitHeld = false;
	/** The driver has asked to box this lap (bit 6, SetPitPlan's box_this_lap),
	 * until the stop is made. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bPitRequested = false;

	/** Each corner's brake, °C, FL FR RL RR (`brake_c`); negative when the
	 * server does not send it. */
	float BrakeTempC[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

	/** The engine's coolant, °C (`water_c`); negative when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float WaterTempC = -1.0f;

	/** The damage, percent: front, rear, left, right, engine (`damage`,
	 * server damage.rs); negative when the server does not send it. A zone
	 * at 100 has put the car out. */
	float DamagePct[5] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};

	/** Where along each zone's face the hits landed, -1 to 1 (`damage_at`):
	 * front and rear across the car (positive left), left and right along it
	 * (positive forward). All 0 with bHasDamageAt false when the server does
	 * not send it, and the whole face is dented. */
	float DamageAt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	bool bHasDamageAt = false;

	/** Each zone's second dent, where a hit landed too far from the first to
	 * fold into it (`damage_at2`), -1 to 1 as DamageAt; NoSecondDent for a
	 * zone with one dent, and from a server that does not send it. */
	static constexpr float NoSecondDent = 9.0f;
	float DamageAt2[4] = {NoSecondDent, NoSecondDent, NoSecondDent, NoSecondDent};

	/**
	 * The soft ground under each tyre that is off the track (`ground`, two
	 * bits a wheel): ApexGround::Gravel, Sand or Desert; None for the road,
	 * a kerb, grass, a wheel in the air, and from an older server.
	 */
	uint8 WheelGround[4] = {0, 0, 0, 0};

	bool HasDamage() const { return DamagePct[0] >= 0.0f; }

	/** The hybrid (server hybrid.rs): the battery's charge and the lap's
	 * deployment budget left, percent; negative when the car has no
	 * hybrid / no budget (or the server does not send them). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float ErsChargePct = -1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float ErsLapPct = -1.0f;

	/** The hybrid's mode (`ApexErs::EMode` as a number), -1 without one. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 ErsMode = -1;

	/** The motor drives / recovers this frame; the overtake button is held. */
	bool bErsDeploying = false;
	bool bErsHarvesting = false;
	bool bErsBoost = false;

	bool HasHybrid() const { return ErsChargePct >= 0.0f; }

	/**
	 * Each tread's inner and outer shoulder, °C, FL FR RL RR (`tyre_c_edges`);
	 * negative when unknown. TyreTempC is the mean of the three tread zones,
	 * so the middle zone is 3 * mean - inner - outer.
	 */
	float TyreInnerC[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
	float TyreOuterC[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

	bool HasTyreEdges() const { return TyreInnerC[0] >= 0.0f; }

	/** Each corner's pad wear, percent (`brake_wear`); negative when unknown. */
	float BrakeWearPct[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

	/**
	 * Each corner's disc wear, percent, apart from its pads (`disc_wear`, on
	 * a car with `[brakes] disc_wear_per_mj`); negative on a car whose discs
	 * never wear, and from a server that predates it.
	 */
	float DiscWearPct[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

	/**
	 * The whole car's stint energy left, percent of `[fuel] stint_energy_mj`
	 * (`stint_energy_pct`); negative without the rule, and from a server
	 * that predates it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float StintEnergyPct = -1.0f;

	/**
	 * The flags race control shows this car (`race_flags`): the blue flag
	 * (a car a lap up close behind), and the yellow in its marshal sector and
	 * the next, 0 none, 1 single, 2 double. Clear outside a race and from a
	 * server that predates them.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	bool bBlueFlag = false;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 YellowAhead = 0;

	/**
	 * The tyre is sliding hard enough to smoke / is locked (`slide_flags`
	 * bits 0-3 and 4-7). False from a server that predates them.
	 */
	bool bTyreSliding[4] = {false, false, false, false};
	bool bTyreLocked[4] = {false, false, false, false};

	/** The hybrid's stint energy budget left, percent (`ers_stint_pct`); negative without a stint rule. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float ErsStintPct = -1.0f;

	/** A recovery's hold left, seconds (`recover_ds`, recovery.rs): 0 for none;
	 *  at least 0.1 while the car waits for traffic to pass. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	float RecoverSecondsLeft = 0.0f;
	bool IsRecovering() const { return RecoverSecondsLeft > 0.0f; }

	/**
	 * How deep a flat spot each tyre carries, 0..1 of the worst, FL FR RL RR
	 * (`flat_spot`, server tyre_thermal.rs); 0 from a server that predates it.
	 * The own car's driver also gets it in FApexDriverFeedback.
	 */
	float FlatSpot[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	/**
	 * The road under each tyre, FL FR RL RR (`tyre_water`, `tyre_marbles`):
	 * the water as a percent of what a downpour leaves on a flat road (over
	 * 100 standing water), and the marbles 0..1. Negative water with
	 * bHasTyreRoad false from a server that predates them (the car's one
	 * figure, the road state's cell under it, stands in).
	 */
	float TyreWaterPct[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
	float TyreMarbles[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	bool bHasTyreRoad = false;
	/** Each tyre's state (`tyre_state`): down to its carcass, or losing air through a slow leak. */
	bool bTyrePunctured[4] = {false, false, false, false};
	bool bTyreLeaking[4] = {false, false, false, false};
	/**
	 * Each tyre's picked-up rubber and blistering, 0..1 (`tyre_pickup`,
	 * `tyre_blister`, the server's TireData): the same for everyone
	 * watching. bHasTyreSurface false from a server that predates them (and
	 * on a stream), when the car works them out itself.
	 */
	float TyrePickup[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float TyreBlister[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	bool bHasTyreSurface = false;

	/**
	 * Each wheel's slip ratio and slip angle in multiples of the tyre's peak
	 * and the surface under it, FL FR RL RR: what the driver feedback tells
	 * a car's own driver, carried for every car by a spectator stream's row
	 * (bHasWheelFeel) so a replay sounds the watched car's tyres and kerbs.
	 * Not in live telemetry.
	 */
	float WheelSlipRatio[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float WheelSlipAngle[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	EApexContactSurface WheelSurface[4] = {EApexContactSurface::Road, EApexContactSurface::Road, EApexContactSurface::Road, EApexContactSurface::Road};
	bool bHasWheelFeel = false;

	/** "S", "M", "H", "I", "W" (server tyre_thermal::COMPOUNDS), or empty when unknown. */
	static FString CompoundLetter(int32 InCompound)
	{
		static const TCHAR* Letters[] = { TEXT("S"), TEXT("M"), TEXT("H"), TEXT("I"), TEXT("W") };
		return InCompound >= 0 && InCompound < static_cast<int32>(UE_ARRAY_COUNT(Letters)) ? Letters[InCompound] : TEXT("");
	}

	/** "SOFT" ... "WET" as the garage names them, or empty when unknown. */
	static FString CompoundName(int32 InCompound)
	{
		static const TCHAR* Names[] = { TEXT("SOFT"), TEXT("MEDIUM"), TEXT("HARD"), TEXT("INTER"), TEXT("WET") };
		return InCompound >= 0 && InCompound < static_cast<int32>(UE_ARRAY_COUNT(Names)) ? Names[InCompound] : TEXT("");
	}

	/**
	 * The compound's name from the car's own list when the setup sheet gave
	 * one (upper-cased, "INTERMEDIATE" rather than the default table's
	 * "INTER"), else the default table's. Empty when unknown either way.
	 */
	static FString CompoundNameFrom(const TArray<FString>& SheetCompounds, int32 InCompound)
	{
		if (SheetCompounds.Num() > 0)
		{
			return SheetCompounds.IsValidIndex(InCompound) ? SheetCompounds[InCompound].ToUpper() : FString();
		}
		return CompoundName(InCompound);
	}

	/**
	 * The first letter of CompoundNameFrom, upper-cased; empty when unknown.
	 * A snow tyre ("SNOW...") is "SN", so it does not read as a soft.
	 */
	static FString CompoundLetterFrom(const TArray<FString>& SheetCompounds, int32 InCompound)
	{
		if (SheetCompounds.Num() > 0)
		{
			const FString Name = CompoundNameFrom(SheetCompounds, InCompound);
			return Name.IsEmpty() ? FString() : Name.StartsWith(TEXT("SNOW")) ? FString(TEXT("SN")) : Name.Left(1);
		}
		return CompoundLetter(InCompound);
	}
};

/**
 * The sky as it is now (`SkyNow`, network.rs), the seventh element of a
 * newer server's `CompactTelemetry`: the day's clock, the weather the
 * forecast has reached and how far the rain and the cloud have come in, the
 * water and the rubber on the road, the air, the wind this moment and the
 * forecast's next change. `bValid` is false from a server that predates it,
 * and then the session's fixed conditions are the sky.
 */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexSkyNow
{
	GENERATED_BODY()

	/** Set when the frame carried a sky. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	bool bValid = false;

	/** The day's clock, seconds after midnight (0..86399). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 ClockS = 13 * 3600;

	/** The weather the forecast has reached. Rain and cloud ease in behind it. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	EApexWeather Weather = EApexWeather::Sunny;

	/** Rain falling, percent of heavy rain. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 RainPct = 0;

	/** Cloud cover, percent. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 CloudPct = 0;

	/** Water on the road, the lap's mean, percent of what heavy rain leaves
	 * on a flat road; over 100 is standing water. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 RoadWaterPct = 0;

	/** The air and the asphalt, °C. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 AirC = 20;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 TrackC = 20;

	/** The wind this moment, gusts included, km/h. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 WindKph = 0;

	/** Where the wind blows TOWARD, degrees counter-clockwise from the server
	 * frame's +X (the track's frame, not the start straight's). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 WindToDeg = 0;

	/** Rubber on the racing line, the lap's mean, percent (50 is what the
	 * cars are calibrated on). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 LineRubberPct = 50;

	/** How fast the day's clock runs (0 stands still). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 TimeScale = 0;

	/** The forecast's next weather; -1 while it holds. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 NextWeather = -1;

	/** Seconds of session until that change; 0 while it holds. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 NextInS = 0;

	/** Snow cover on the racing line, the lap's mean, percent; 0 without snow
	 * (and from a server that predates it). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 LineSnowPct = 0;

	/** Snow falling, percent of steady snowfall. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 SnowfallPct = 0;

	/** The second and third changes the forecast holds after the next, and the
	 * seconds until each (0 when it holds no more). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 Next2Weather = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 Next2InS = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 Next3Weather = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Sky")
	int32 Next3InS = 0;

	bool HasNextChange() const { return NextWeather >= 0 && NextInS > 0; }
	bool HasNext2Change() const { return HasNextChange() && Next2InS > 0 && Next2Weather < FApexSessionConditions::WeatherCount; }
	bool HasNext3Change() const { return HasNext2Change() && Next3InS > 0 && Next3Weather < FApexSessionConditions::WeatherCount; }

	/** Hours since midnight, fractional. */
	float Hours() const { return static_cast<float>(ClockS) / 3600.0f; }

	/** "15:30". */
	FString ClockText() const
	{
		const int32 Minutes = (ClockS / 60) % FApexSessionConditions::MinutesPerDay;
		return FString::Printf(TEXT("%02d:%02d"), Minutes / 60, Minutes % 60);
	}

	/** The wind as a vector in the server frame, m/s toward where it blows. */
	FVector2D WindServerMps() const
	{
		const float Speed = static_cast<float>(WindKph) / 3.6f;
		const float Rad = FMath::DegreesToRadians(static_cast<float>(WindToDeg));
		return FVector2D(Speed * FMath::Cos(Rad), Speed * FMath::Sin(Rad));
	}

	/**
	 * This sky as session conditions, for the sky model: the clock to the
	 * minute and the forecast's weather. The air is named from the frame.
	 */
	FApexSessionConditions AsConditions(const FApexSessionConditions& Session) const
	{
		FApexSessionConditions Out = Session;
		Out.Weather = Weather;
		Out.TimeOfDayMinutes = (ClockS / 60) % FApexSessionConditions::MinutesPerDay;
		Out.AirTempC = AirC;
		Out.WindKph = WindKph;
		return Out.Clamped();
	}

	bool operator==(const FApexSkyNow& Other) const
	{
		return bValid == Other.bValid && ClockS == Other.ClockS && Weather == Other.Weather
			&& RainPct == Other.RainPct && CloudPct == Other.CloudPct && RoadWaterPct == Other.RoadWaterPct
			&& AirC == Other.AirC && TrackC == Other.TrackC && WindKph == Other.WindKph
			&& WindToDeg == Other.WindToDeg && LineRubberPct == Other.LineRubberPct
			&& TimeScale == Other.TimeScale && NextWeather == Other.NextWeather && NextInS == Other.NextInS
			&& LineSnowPct == Other.LineSnowPct && SnowfallPct == Other.SnowfallPct
			&& Next2Weather == Other.Next2Weather && Next2InS == Other.Next2InS
			&& Next3Weather == Other.Next3Weather && Next3InS == Other.Next3InS;
	}
	bool operator!=(const FApexSkyNow& Other) const { return !(*this == Other); }
};

/**
 * The safety car on the road (`CompactTelemetry.safety_car`, sim/src/safety_car.rs):
 * `[x, y, z, yaw_rad, speed_mps]` in the server's frame, as a car's pose is
 * sent. Not a participant: it has no roster row.
 */
struct FApexSafetyCarPose
{
	/** False while there is none on the road (and from an older server). */
	bool bValid = false;
	FVector Position = FVector::ZeroVector;
	float YawRad = 0.0f;
	float SpeedMps = 0.0f;
};

/** `CompactTelemetry.race_phase` values (sim/src/race_start.rs `RacePhase`). */
namespace ApexRacePhase
{
	inline constexpr int32 None = 0;
	inline constexpr int32 Formation = 1;
	inline constexpr int32 PaceLap = 2;
	inline constexpr int32 Vsc = 3;
	inline constexpr int32 VscEnding = 4;
	inline constexpr int32 SafetyCar = 5;
	inline constexpr int32 ScInThisLap = 6;
}

/** `CompactTelemetry` (network.rs:415) — positional encoding, UDP. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexTelemetryFrame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int64 ServerTick = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	EApexSessionState SessionState = EApexSessionState::Lobby;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	EApexGameMode GameMode = EApexGameMode::Lobby;

	/** -1 when the server sent nil. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 CountdownMs = -1;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	TArray<FApexCarTelemetry> Cars;

	/**
	 * A timed race's clock (`RaceClock`, appended): ms of race time left, 0
	 * once it has run out; -1 in any other session and from an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 RaceLeftMs = -1;

	/** The lap a timed race ends on, once its clock has run out; 0 before (and in any other session). */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	int32 RaceFinalLap = 0;

	bool HasRaceClock() const { return RaceLeftMs >= 0; }

	/**
	 * The sky now (`SkyNow`, appended after the race clock, which is then
	 * nil when there is none). `Sky.bValid` is false from an older server.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Race")
	FApexSkyNow Sky;

	/**
	 * The race's marshal sectors showing a yellow (`yellow_flags`, appended
	 * after the sky; absent while none is out): each sector is
	 * YellowSectorM metres of lap from `Sector * YellowSectorM`, its level 1
	 * single, 2 double.
	 */
	int32 YellowSectorM = 0;
	TArray<TPair<int32, int32>> YellowSectors;

	/**
	 * What the race is doing beyond its state and mode (`race_phase`,
	 * appended after the yellows): an `ApexRacePhase` value, 0 none.
	 */
	int32 RacePhase = ApexRacePhase::None;

	/** The safety car's pose (`safety_car`, appended after the phase). */
	FApexSafetyCarPose SafetyCar;

	/** The yellow over lap position `ProgressM`, 0 none. */
	int32 YellowAt(float ProgressM) const
	{
		if (YellowSectorM <= 0)
		{
			return 0;
		}
		const int32 Sector = FMath::FloorToInt(FMath::Max(ProgressM, 0.0f) / YellowSectorM);
		for (const TPair<int32, int32>& Yellow : YellowSectors)
		{
			if (Yellow.Key == Sector)
			{
				return Yellow.Value;
			}
		}
		return 0;
	}
};

/** What is under one tyre (`feedback::ContactSurface`). Ordered by roughness. */

/** One wheel's share of FApexDriverFeedback. Transients are the peak since the previous message. */
struct FApexWheelFeedback
{
	/** Longitudinal slip in multiples of the tyre's peak slip ratio, negative braking. Past ±1 the wheel is locking or spinning. */
	float SlipRatio = 0.0f;

	/** Slip angle in multiples of the tyre's peak slip angle. Past ±1 the tyre is sliding. */
	float SlipAngle = 0.0f;

	/** The roughest surface the tyre touched since the previous message. */
	EApexContactSurface Surface = EApexContactSurface::Road;

	/** Suspension compression speed, m/s (positive compressing), largest magnitude: bumps and landings. */
	float SuspensionMps = 0.0f;

	/**
	 * How deep a flat spot a locked wheel has ground into the tyre, 0..1 of
	 * the worst (server tyre_thermal.rs): felt as a shake once a turn of the
	 * wheel. 0 from a server that predates the field.
	 */
	float FlatSpot = 0.0f;
};

/**
 * `SessionPhysics` (network.rs, `SessionJoined.Physics`): what the client's
 * copy of the car step needs from the server to step the own car as the
 * session does (docs/proposals/client-prediction.md). Sent to a driver;
 * absent (not bValid) for a spectator, a demo, and from a server that
 * predates it, which leaves prediction off.
 */
struct FApexSessionPhysics
{
	bool bValid = false;
	/** The session's tick rate, Hz: the step's dt is its inverse. */
	int32 TickRateHz = 0;
	/** 0 the centerline, 1 the road mesh (apx_track_load's road_contact). */
	int32 RoadContact = 0;
	/**
	 * The checksum of the track's physics files, the YAML and every sidecar
	 * (sim/src/track_bundle.rs), naming the bundle RequestTrackBundle asks
	 * for; its size deflated and its chunks. 0 when the server serves none
	 * (an older server, a track built in memory).
	 */
	uint32 BundleCrc = 0;
	int64 BundleBytes = 0;
	int32 BundleChunks = 0;
};

/**
 * `OwnCarSnapshot` (sim/src/own_car.rs): the driver's own car at a tick,
 * with the newest input sequence the server had applied by then. Sent in
 * `DriverFeedback` to a client that numbers its inputs
 * (docs/proposals/client-prediction.md): what the prediction rewinds to.
 * Everything is in the SERVER frame and units (metres, +Y left, yaw
 * counter-clockwise), exactly as the server's f32s, because it goes back
 * into the server's physics library unchanged. Per-wheel arrays are FL, FR,
 * RL, RR.
 */
struct FApexOwnCarSnapshot
{
	static constexpr uint8 FlagOnTrack = 1;
	static constexpr uint8 FlagAirborne = 2;
	static constexpr uint8 FlagColliding = 4;
	static constexpr uint8 FlagDrsOpen = 8;
	static constexpr uint8 FlagWheelsOffTrack = 16;

	/** The session tick the car is at. */
	int64 Tick = -1;
	/** The newest PlayerInput sequence applied by Tick; -1 when none was. */
	int64 InputSeq = -1;
	float Pos[3] = {};
	/** Yaw, pitch, roll, rad. */
	float Attitude[3] = {};
	float Vel[3] = {};
	float SpeedMps = 0.0f;
	/** Yaw, pitch and roll rates, rad/s. */
	float AngularVel[3] = {};
	float WheelAngularVel[4] = {};
	float WheelLoadN[4] = {};
	float SuspensionTravelM[4] = {};
	float SuspensionVelocityMps[4] = {};
	/** Lateral, longitudinal, vertical. */
	float GForces[3] = {};
	float VerticalAccelMps2 = 0.0f;
	/** Along and across the car. */
	float GroundSlope[2] = {};
	/** Phase and amplitude. */
	float Porpoise[2] = {};
	/** Throttle, brake, steering (SERVER sign, positive left) and clutch as the car applied them. */
	float Controls[4] = {};
	int32 Gear = 0;
	float EngineRpm = 0.0f;
	float TurboSpool = 0.0f;
	int32 AutoShiftHoldTicks = 0;
	int32 AutoReverseTicks = 0;
	float TrackProgress = 0.0f;
	float LateralOffsetM = 0.0f;
	int32 CurrentLap = 0;
	/** The centerline point the server's windowed search starts from; -1 none. */
	int64 CenterlineIdx = -1;
	/** Flag* bits. */
	uint8 Flags = 0;
};

/**
 * `StateChunk` (sim/src/own_car.rs): one slice of the own car's full
 * state blob (`car_step::encode_state`, "APXC"), sent a slice a feedback
 * message, a blob every half second. FApexOwnCarStateAssembler puts them
 * back together.
 */
struct FApexOwnCarStateChunk
{
	/** The tick the blob is the car at, and the newest input applied by then (-1 none). */
	int64 Tick = -1;
	int64 InputSeq = -1;
	int32 Index = 0;
	int32 Count = 0;
	TArray<uint8> Bytes;
};

/**
 * Puts the own car's state blob back together from its slices, in order. A
 * slice that does not follow the one before (lost, or a newer blob begun)
 * drops the blob in progress; the next one starts with slice 0.
 */
struct FApexOwnCarStateAssembler
{
	/** The newest whole blob, the tick it is the car at and the input applied by then; Tick -1 before the first. */
	int64 Tick = -1;
	int64 InputSeq = -1;
	TArray<uint8> Blob;

	/** Takes the next slice. True when it completed a blob, now in Blob. */
	bool Add(const FApexOwnCarStateChunk& Chunk)
	{
		if (Chunk.Index == 0)
		{
			PendingTick = Chunk.Tick;
			PendingSeq = Chunk.InputSeq;
			PendingCount = Chunk.Count;
			Pending.Reset();
			NextIndex = 0;
		}
		if (Chunk.Count <= 0 || Chunk.Index != NextIndex || Chunk.Tick != PendingTick
			|| Chunk.Count != PendingCount)
		{
			NextIndex = -1;
			return false;
		}
		Pending.Append(Chunk.Bytes);
		if (++NextIndex < PendingCount)
		{
			return false;
		}
		Tick = PendingTick;
		InputSeq = PendingSeq;
		Blob = MoveTemp(Pending);
		Pending.Reset();
		NextIndex = -1;
		return true;
	}

	void Reset() { *this = FApexOwnCarStateAssembler(); }

private:
	int64 PendingTick = -1;
	int64 PendingSeq = -1;
	int32 PendingCount = 0;
	int32 NextIndex = -1;
	TArray<uint8> Pending;
};

/**
 * `DriverFeedback` (sim/src/feedback.rs) - positional encoding, UDP only,
 * sent to a car's own driver with every telemetry frame. It carries what the
 * driver should feel, whatever the device: a wheel's constant force comes
 * from SteerTorque, and a pad's rumble from the slip, surface and hits.
 */
struct APEXSIMNET_API FApexDriverFeedback
{
	static constexpr int32 FrontLeft = 0;
	static constexpr int32 FrontRight = 1;
	static constexpr int32 RearLeft = 2;
	static constexpr int32 RearRight = 3;

	/** Tick of the newest sample; 0 until a message arrives. */
	int64 ServerTick = 0;

	/**
	 * Steering-column torque for every physics tick since the previous
	 * message, oldest first (7 at the default 420 Hz sim and 60 Hz telemetry).
	 * SERVER sign: positive turns the wheel LEFT. 1.0 is the car's reference,
	 * the front axle at its static grip limit; downforce takes it past 1.
	 */
	TArray<float, TInlineAllocator<8>> SteerTorque;

	/** FL, FR, RL, RR. */
	FApexWheelFeedback Wheels[4];

	/** ABS held a wheel back from locking during the interval. */
	bool bAbsActive = false;

	/** Traction control cut drive to a wheel during the interval. */
	bool bTcActive = false;

	/** Closing speed of the hardest contact with another car in the interval, m/s; 0 when none. */
	float ImpactMps = 0.0f;

	/**
	 * The hardest jolt a hit put through the steering column in the interval,
	 * in SteerTorque's units and SERVER sign (positive turns the wheel left);
	 * 0 when none, and from a server that predates the field.
	 */
	float SteerKick = 0.0f;

	/**
	 * The steering input the newest SteerTorque sample was worked out at,
	 * -1..1, SERVER sign (positive left). With SteerStiffness it lets a wheel
	 * correct that sample for where the rim is now.
	 */
	float SteerInput = 0.0f;

	/**
	 * How the torque changes per unit of steering input around SteerInput,
	 * in SteerTorque's units. The same in either sign convention (both the
	 * torque and the input flip). Negative while the fronts grip, positive
	 * past the aligning crest; 0 from a server that predates the field, which
	 * turns the correction off.
	 */
	float SteerStiffness = 0.0f;

	/** Front axle load over its static share at the newest sample: above 1 under braking and with downforce. */
	float FrontLoad = 1.0f;

	/**
	 * The own car at ServerTick, for prediction: only for a client that
	 * numbers its inputs, and not from a server that predates it.
	 */
	TOptional<FApexOwnCarSnapshot> OwnCar;

	/** A slice of the own car's state blob, when one is going out. */
	TOptional<FApexOwnCarStateChunk> OwnState;

	/**
	 * Not on the wire: FPlatformTime::Seconds when the UDP worker took the
	 * datagram in (after any fake latency), which times the prediction's
	 * clock closer than the game frame that reads it.
	 */
	double ReceivedSeconds = 0.0;

	/**
	 * Folds the next message in, as if the server had sent one message for
	 * both intervals: samples appended, peaks kept, the roughest surface.
	 * Used when several arrive between two game frames, so a kerb strike in
	 * the first is not lost to the second.
	 */
	void Absorb(const FApexDriverFeedback& Next)
	{
		auto Peak = [](float Held, float New) { return FMath::Abs(New) > FMath::Abs(Held) ? New : Held; };
		ServerTick = Next.ServerTick;
		SteerTorque.Append(Next.SteerTorque);
		for (int32 Wheel = 0; Wheel < 4; ++Wheel)
		{
			FApexWheelFeedback& Mine = Wheels[Wheel];
			const FApexWheelFeedback& Theirs = Next.Wheels[Wheel];
			Mine.SlipRatio = Peak(Mine.SlipRatio, Theirs.SlipRatio);
			Mine.SlipAngle = Peak(Mine.SlipAngle, Theirs.SlipAngle);
			Mine.SuspensionMps = Peak(Mine.SuspensionMps, Theirs.SuspensionMps);
			Mine.Surface = FMath::Max(Mine.Surface, Theirs.Surface);
			Mine.FlatSpot = Theirs.FlatSpot;
		}
		bAbsActive |= Next.bAbsActive;
		bTcActive |= Next.bTcActive;
		ImpactMps = FMath::Max(ImpactMps, Next.ImpactMps);
		SteerKick = Peak(SteerKick, Next.SteerKick);
		// These belong with the newest torque sample.
		SteerInput = Next.SteerInput;
		SteerStiffness = Next.SteerStiffness;
		FrontLoad = Next.FrontLoad;
		// The snapshot is a snapshot: the newest wins. A state slice is only
		// whole with its neighbours, so the net subsystem takes each one off
		// before folding the messages together.
		if (Next.OwnCar.IsSet())
		{
			OwnCar = Next.OwnCar;
		}
		OwnState = Next.OwnState;
	}
};

/** The player's control inputs, sent over UDP at frame rate. */
USTRUCT(BlueprintType)
struct APEXSIMNET_API FApexPlayerInput
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	float Throttle = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	float Brake = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	float Steering = 0.0f;

	/** Negative leaves the gear alone (the protocol's `None`). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 Gear = -128;

	/**
	 * The DRS button is held. The server decides whether the flap opens
	 * (only in a zone the car earned; `FApexCarTelemetry::bDrsAllowed`).
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bDrs = false;

	/**
	 * The headlight switch: -1 leaves the lights to the session's sky (sent
	 * as nil; on at dusk, at night and in the rain), 0 off, 1 on.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 Headlights = -1;

	/** The flash button is held: full beam, whatever the switch says. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bFlash = false;

	/** The hybrid's mode (`ApexErs::EMode`: 0 harvest, 1 balanced, 2
	 * attack); -1 is sent as nil and keeps the car's. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	int32 ErsMode = -1;

	/** The overtake button is held: full deployment whatever the mode. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bErsBoost = false;

	/** The handbrake is held: the rear tyres lock (`PlayerInput.handbrake`). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Race")
	bool bHandbrake = false;

	/**
	 * The clutch pedal, 0 released .. 1 pressed (`PlayerInput.clutch`: the
	 * crank transmits proportionally less torque); negative when the shell
	 * has no clutch pedal, and then nil on the wire, which the server takes
	 * as "leave it alone" (the auto gearbox's own).
	 */
	float Clutch = -1.0f;
	bool HasClutch() const { return Clutch >= 0.0f; }

	bool HasGear() const { return Gear != -128; }
};

/** The kind of a decoded server message. */
enum class EApexServerMessageType : uint8
{
	Unknown,
	AuthSuccess,
	AuthFailure,
	HeartbeatAck,
	LobbyState,
	SessionJoined,
	SessionLeft,
	SessionStarting,
	GameModeChanged,
	CountdownUpdate,
	Error,
	PlayerDisconnected,
	SessionRoster,
	RacingLine,
	TrackSectors,
	TrackCorners,
	LapTiming,
	LapRecord,
	GhostLap,
	QualifyingResults,
	CarSetupSheet,
	PitService,
	Showcases,
	SpectatorJoined,
	/** A run of spectator stream records (TCP: framed in `SpectatorRecords`; UDP: one frame body, framed on the way in). */
	SpectatorRecord,
	UdpHandshakeAck,
	TelemetryCompact,
	DriverFeedback,
	/** A race weekend's phase, round and knockouts. */
	WeekendState,
	/** Full named-encoding Telemetry — replays only; the wire uses the compact form. */
	IgnoredVariant,
	/** A seat is held for us in a session we lost the connection to (SessionId, TrackId, SessionKind). */
	RejoinAvailable,
	/** A slice of the session's road state (`RoadState`). */
	RoadState,
	/** A track's fastest laps, on request (`Leaderboard`). */
	Leaderboard,
	/** The local driver's own pit box (`PitBox`). */
	PitBox,
	/** An operator's message to the player (`ServerNotice`; text in Reason). */
	ServerNotice,
	/** A qualifying shoot-out's run order and progress (`ShootoutState`). */
	ShootoutState,
	/** A chat line from a player (`Chat`). */
	Chat,
	/** A chunk of the track's physics bundle, asked for with RequestTrackBundle (`TrackBundleChunk`). */
	TrackBundleChunk,
};

/**
 * `TrackBundleChunkData` (network.rs): one chunk of a track's physics bundle
 * (sim/src/track_bundle.rs), answering RequestTrackBundle. Chunks 0 says
 * the server has no bundle with that checksum, or no such chunk.
 */
struct FApexTrackBundleChunk
{
	uint32 Crc = 0;
	int32 Chunk = 0;
	int32 Chunks = 0;
	TArray<uint8> Bytes;
};

/** A chat line as the server sends it on (`ChatData`). */
struct APEXSIMNET_API FApexChatLine
{
	FString PlayerId;
	/** The sender's name as the lobby lists it. */
	FString From;
	FString Text;
	/** Said in a session (to its drivers and watchers), not the lobby. */
	bool bInSession = false;
	/** FPlatformTime::Seconds when it arrived, for fading it out. */
	double ArrivedSeconds = 0.0;
	bool bValid = false;
};

/**
 * One decoded server message. A tagged union flattened into a struct: the set
 * of payloads is small and fixed, and a flat struct keeps the queue between the
 * socket thread and the game thread free of virtual dispatch.
 */
struct APEXSIMNET_API FApexServerMessage
{
	EApexServerMessageType Type = EApexServerMessageType::Unknown;

	/** The raw variant name, kept for logging unknown/ignored variants. */
	FString VariantName;

	FApexAuthSuccess AuthSuccess;
	FApexLobbyState LobbyState;
	FApexSessionRoster Roster;
	FApexRacingLineData RacingLine;
	FApexTrackSectors TrackSectors;
	FApexTrackCorners TrackCorners;
	FApexRoadState RoadState;
	FApexLapTiming LapTiming;
	FApexLapRecord LapRecord;
	FApexGhostLap GhostLap;
	/** QualifyingResults: the track they are for, and the results newest first. */
	FString QualifyingTrackId;
	TArray<FApexQualifyingResult> QualifyingResults;
	/** Leaderboard: the track and car (empty: every car) it is for, best lap first. */
	FString LeaderboardTrackId;
	FString LeaderboardCarId;
	TArray<FApexLeaderboardEntry> Leaderboard;
	FApexCarSetupSheet CarSetupSheet;
	FApexPitService PitService;
	FApexTelemetryFrame Telemetry;
	FApexDriverFeedback DriverFeedback;
	FApexTrackBundleChunk TrackBundleChunk;

	/** Showcases::entries. */
	TArray<FApexShowcaseSummary> Showcases;
	/** SpectatorJoined. */
	FString StreamId;
	EApexSpectatorKind SpectatorKind = EApexSpectatorKind::Showcase;
	FString ShowcaseId;
	/** SpectatorRecord: stream records in the stream's own `[u32 length][body]` framing. */
	TArray<uint8> SpectatorRecords;

	/** AuthFailure::reason, Error::message, or ServerNotice::message. */
	FString Reason;
	/** SessionJoined::SessionId, or RejoinAvailable::SessionId. */
	FString SessionId;
	/** RejoinAvailable::TrackId. */
	FString TrackId;
	/** PlayerDisconnected::PlayerId. */
	FString PlayerId;

	int32 GridPosition = 0;
	/** SessionJoined::SessionKind; Multiplayer from a server that predates the field. */
	EApexSessionKind SessionKind = EApexSessionKind::Multiplayer;
	/** SessionJoined::AllowedAssists; everything allowed from a server that predates the field. */
	FApexAllowedAssists AllowedAssists;
	/** SessionJoined::Conditions; a sunny afternoon from a server that predates the field. */
	FApexSessionConditions Conditions;
	/** SessionJoined::Damage, the session's damage rule; full when absent. */
	EApexDamageLevel Damage = EApexDamageLevel::Full;
	/** SessionJoined::AiSkill, the AI field's level (ApexAiSkill); Mixed when absent. */
	int32 AiSkill = ApexAiSkill::Mixed;
	/** SessionJoined::RaceSeconds, a timed race's length (ApexRaceLength); 0 for a race over laps. */
	int32 RaceSeconds = 0;
	/** SessionJoined::QualifyingSeconds, a timed qualifying's length (ApexQualifyingLength); 0 for open qualifying. */
	int32 QualifyingSeconds = 0;
	/** SessionJoined::Weekend; no rounds when the session is no race weekend. */
	FApexWeekendFormat Weekend;
	/** SessionJoined::QualifyingCutoffPct, the 107% rule (ApexQualifyingCutoff); 0 for none. */
	int32 QualifyingCutoffPct = 0;
	/** SessionJoined::MandatoryStops, the race's pit rule (ApexMandatoryStops); 0 for none. */
	int32 MandatoryStops = 0;
	/** SessionJoined::CompoundChange: the race must be run on two dry compounds. */
	bool bCompoundChange = false;
	/** SessionJoined::QualifyingFormat: Shootout for a one-lap shoot-out. */
	EApexQualifyingFormat QualifyingFormat = EApexQualifyingFormat::Open;
	/** SessionJoined::QualifyingLapAllowance (ApexQualifyingLapAllowance); 0 for none. */
	int32 QualifyingLapAllowance = 0;
	/** SessionJoined::Physics: a driver's session's tick rate and road; not bValid when absent. */
	FApexSessionPhysics Physics;
	/**
	 * SessionJoined::Conditions as the server wrote them (MessagePack, named):
	 * what the physics library's apx_track_load bakes, every field kept,
	 * including those FApexSessionConditions does not hold.
	 */
	TArray<uint8> ConditionsBytes;
	/** WeekendState. */
	FApexWeekendState WeekendState;
	/** ShootoutState. */
	FApexShootoutState ShootoutState;
	FApexPitBox PitBox;
	FApexChatLine Chat;
	int32 CountdownSeconds = 0;
	int64 ServerTick = 0;
	int32 ErrorCode = 0;
	EApexGameMode GameMode = EApexGameMode::Lobby;
};
