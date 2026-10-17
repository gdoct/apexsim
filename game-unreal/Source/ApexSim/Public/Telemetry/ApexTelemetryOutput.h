#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexTelemetryOutput.generated.h"

struct FApexHudData;
class FSocket;
class FInternetAddr;

/**
 * Telemetry output for external apps and rigs (docs/game/telemetry-output.md):
 * SimHub, motion platforms, bass shakers, wheel rev lights, dash apps.
 *
 * One fixed, little-endian UDP datagram, sent from the client (it has the
 * player's car at telemetry rate) to an address in settings.yml
 * (`telemetry_out`). The layout is a public API: append-only, like the wire.
 * A field is never moved or retyped; a new one goes at the end and raises
 * `Version`, and `Size` says how much of the packet a reader may trust.
 */
namespace ApexTelemetryOutput
{
	/** "APXT" read as a little-endian u32. */
	inline constexpr uint32 Magic = 0x54585041u;
	inline constexpr uint16 Version = 1;
	/** Bytes of a version-1 packet. */
	inline constexpr uint16 PacketBytes = 216;

	/** `Flags` bits. */
	enum EFlag : uint32
	{
		/** A car is being reported: everything below the header is about it. Clear in the menus. */
		CarPresent    = 1u << 0,
		/** The car is the one being watched, not driven (a spectator, a replay). */
		Spectating    = 1u << 1,
		/** A saved replay or a timeshift is playing. */
		Replay        = 1u << 2,
		OnTrack       = 1u << 3,
		InPitLane     = 1u << 4,
		InGarage      = 1u << 5,
		/** ABS held a wheel back during the last feedback interval (driven car only). */
		AbsActive     = 1u << 6,
		/** Traction control cut drive during the last feedback interval (driven car only). */
		TcActive      = 1u << 7,
		DrsAllowed    = 1u << 8,
		DrsOpen       = 1u << 9,
		Headlights    = 1u << 10,
		YellowFlag    = 1u << 11,
		BlueFlag      = 1u << 12,
		/** The lap in progress has been struck for track limits. */
		LapInvalid    = 1u << 13,
		/** The car has taken the chequered flag. */
		Finished      = 1u << 14,
		/** Motion is known: two telemetry frames of this car have been seen. */
		MotionValid   = 1u << 15,
	};

	/**
	 * Everything a packet carries, in its units. Encode writes it in the
	 * order and at the offsets docs/game/telemetry-output.md lists.
	 * Unknown values are -1 unless said otherwise.
	 */
	struct APEXSIM_API FPacket
	{
		uint32 Sequence = 0;
		uint32 Flags = 0;
		/** Seconds since the sender started. */
		float TimeS = 0.0f;
		/** EApexGameMode as an int (docs/game/telemetry-output.md lists them). */
		int32 GameMode = 0;

		// Motion. Server frame: x and y level, z up, metres. The car's own
		// axes: x forward, y left, z up.
		FVector3f Position = FVector3f::ZeroVector;
		/** Yaw (from +x toward +y), pitch (nose up positive), roll (leaning right positive), rad. */
		FVector3f Attitude = FVector3f::ZeroVector;
		/** m/s in the car's axes. */
		FVector3f LocalVelocity = FVector3f::ZeroVector;
		/** m/s^2 in the car's axes, gravity left out: surge, sway, heave. */
		FVector3f LocalAccel = FVector3f::ZeroVector;
		/** rad/s: roll rate, pitch rate, yaw rate, as the attitude angles change. */
		FVector3f AngularRate = FVector3f::ZeroVector;

		// Car.
		float SpeedMps = 0.0f;
		float Rpm = 0.0f;
		/** The redline and the limiter (0 when the car's rev range is not known). */
		float RedlineRpm = 0.0f;
		float MaxRpm = 0.0f;
		/** Rev lights: 0 to 1 over the car's rev range, as the HUD's car.rpm_fraction. */
		float RevFraction = 0.0f;
		/** -1 reverse, 0 neutral, 1.. forward. */
		int32 Gear = 0;
		float Throttle = 0.0f;
		float Brake = 0.0f;
		/** -1 full left to 1 full right. */
		float Steering = 0.0f;
		float FuelLiters = -1.0f;
		/** 0 to 1; -1 for a car with no hybrid. */
		float ErsCharge = -1.0f;
		/** FL, FR, RL, RR. */
		float TyreTempC[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
		float TyrePressureKpa[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
		float TyreWearPct[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

		// Session.
		/** The car's place, 1 for the lead; 0 when not known. */
		int32 Place = 0;
		int32 CarCount = 0;
		int32 Lap = 0;
		/** 0 for no lap limit (a timed race, practice, a hotlap). */
		int32 LapLimit = 0;
		float LapTimeS = 0.0f;
		/** 0 for none yet. */
		float LastLapS = 0.0f;
		float BestLapS = 0.0f;
		/** Against the reference lap, s; 0 with none. */
		float DeltaS = 0.0f;
		/** Metres round the lap from the line (the HUD's car.station_m). */
		float StationM = 0.0f;
		/** A timed race's time left, s; -1 in any other session. */
		float RaceLeftS = -1.0f;
	};

	/** The packet's bytes: exactly PacketBytes, little-endian. */
	APEXSIM_API TArray<uint8> Encode(const FPacket& Packet);

	/**
	 * Fills the car and session fields from the HUD's data points (the same
	 * names a content/hud component reads), so the output and the HUD are one
	 * API. Motion and the feedback-only flags are left to the caller.
	 */
	APEXSIM_API void FillFromHud(const FApexHudData& Hud, FPacket& Out);

	/**
	 * Motion from the telemetry stream: velocity and acceleration from the
	 * car's position tick to tick, rates from its attitude. Time is the server's
	 * ticks, whose length is learnt from their arrival (as the motion buffer
	 * does), so jittery arrival does not shake a rig. Acceleration is smoothed
	 * by a one-pole filter.
	 */
	class APEXSIM_API FMotionEstimator
	{
	public:
		/** Forget the car: the next sample starts again (a new car, a teleport). */
		void Reset();

		/**
		 * One telemetry sample of the car. A tick that does not move on is
		 * ignored; one that runs back, or a jump of more than 50 m, restarts.
		 */
		void AddSample(int64 ServerTick, double ArrivalSeconds, const FVector3f& Position, const FVector3f& Attitude);

		/** Velocity and attitude known (two samples). */
		bool HasMotion() const { return Samples >= 2; }

		/** Writes Position, Attitude, LocalVelocity, LocalAccel and AngularRate. */
		void Fill(FPacket& Out) const;

		/** The smoothing's corner frequency for acceleration and rates, Hz. */
		float FilterHz = 8.0f;

		/** Seconds per server tick as learnt; starts at 1/420. */
		double SecondsPerTick() const { return TickSeconds; }

	private:
		int32 Samples = 0;
		int64 LastTick = 0;
		double LastArrival = 0.0;
		FVector3f LastPosition = FVector3f::ZeroVector;
		FVector3f LastAttitude = FVector3f::ZeroVector;
		FVector3f WorldVelocity = FVector3f::ZeroVector;
		FVector3f WorldAccel = FVector3f::ZeroVector;
		FVector3f Rates = FVector3f::ZeroVector;
		double TickSeconds = 1.0 / 420.0;
		/** The arrivals' slow mean once TickSeconds is held: a server of another rate. */
		double DriftSeconds = 1.0 / 420.0;
		int32 TickMeasurements = 0;
		/** About a second of telemetry. */
		static constexpr int32 TickLearnSamples = 60;
	};

	/** The car's own axes (forward, left, up) in the server frame for an attitude. */
	APEXSIM_API void CarAxes(const FVector3f& Attitude, FVector3f& OutForward, FVector3f& OutLeft, FVector3f& OutUp);
}

/** `telemetry_out` in settings.yml. */
struct FApexTelemetryOutputSettings
{
	bool bEnabled = false;
	FString Host = TEXT("127.0.0.1");
	int32 Port = 9996;
	/** Packets per second, 1 to 400. */
	int32 RateHz = 60;
};

/**
 * Sends the packet while enabled: at RateHz, in a race view (with
 * CarPresent) and in the menus (without, at 2 Hz, so an app can tell the
 * game is up). `apexsim.telemetry.Out [on|off]` switches it for this run.
 */
UCLASS()
class APEXSIM_API UApexTelemetryOutputSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UApexTelemetryOutputSubsystem();
	virtual ~UApexTelemetryOutputSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void Configure(const FApexTelemetryOutputSettings& InSettings);
	const FApexTelemetryOutputSettings& GetSettings() const { return Settings; }
	uint32 GetPacketsSent() const { return Sequence; }

private:
	bool Tick(float DeltaSeconds);
	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);
	void OpenSocket();
	void CloseSocket();
	void Send();

	FApexTelemetryOutputSettings Settings;
	FTSTicker::FDelegateHandle TickerHandle;
	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> Destination;
	ApexTelemetryOutput::FMotionEstimator Motion;
	int32 MotionCarIndex = -1;
	uint32 Sequence = 0;
	double StartSeconds = 0.0;
	double SinceSend = 0.0;
	bool bWarnedSend = false;
};
