#include "Telemetry/ApexTelemetryOutput.h"

#include "ApexBootSettings.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Common/UdpSocketBuilder.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Hud/ApexHudDataSubsystem.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

// --- Packet ------------------------------------------------------------------

namespace
{
	/** Little-endian writes, whatever the platform. */
	struct FTelemetryWriter
	{
		TArray<uint8>& Bytes;

		void U16(uint16 V)
		{
			Bytes.Add(static_cast<uint8>(V));
			Bytes.Add(static_cast<uint8>(V >> 8));
		}
		void U32(uint32 V)
		{
			for (int32 Shift = 0; Shift < 32; Shift += 8)
			{
				Bytes.Add(static_cast<uint8>(V >> Shift));
			}
		}
		void I32(int32 V) { U32(static_cast<uint32>(V)); }
		void F32(float V)
		{
			uint32 Bits = 0;
			FMemory::Memcpy(&Bits, &V, sizeof(Bits));
			U32(Bits);
		}
		void Vec(const FVector3f& V)
		{
			F32(V.X);
			F32(V.Y);
			F32(V.Z);
		}
		void Four(const float (&V)[4])
		{
			for (const float X : V)
			{
				F32(X);
			}
		}
	};

	/** A data point as a number, or Fallback when the game could not fill it. */
	float TelemetryNumber(const FApexHudData& Hud, const TCHAR* Name, float Fallback)
	{
		const FApexHudValue* Value = Hud.Find(FName(Name));
		return Value && !Value->IsNone() ? static_cast<float>(Value->AsNumber()) : Fallback;
	}

	bool TelemetryBool(const FApexHudData& Hud, const TCHAR* Name)
	{
		const FApexHudValue* Value = Hud.Find(FName(Name));
		return Value && !Value->IsNone() && Value->AsBool();
	}

	/** Unwraps an angle step across the +-pi seam. */
	float TelemetryAngleStep(float From, float To)
	{
		return FMath::UnwindRadians(To - From);
	}
}

TArray<uint8> ApexTelemetryOutput::Encode(const FPacket& P)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(PacketBytes);
	FTelemetryWriter W{Bytes};

	// Header.
	W.U32(Magic);
	W.U16(Version);
	W.U16(PacketBytes);
	W.U32(P.Sequence);
	W.U32(P.Flags);
	W.F32(P.TimeS);
	W.I32(P.GameMode);

	// Motion.
	W.Vec(P.Position);
	W.Vec(P.Attitude);
	W.Vec(P.LocalVelocity);
	W.Vec(P.LocalAccel);
	W.Vec(P.AngularRate);

	// Car.
	W.F32(P.SpeedMps);
	W.F32(P.Rpm);
	W.F32(P.RedlineRpm);
	W.F32(P.MaxRpm);
	W.F32(P.RevFraction);
	W.I32(P.Gear);
	W.F32(P.Throttle);
	W.F32(P.Brake);
	W.F32(P.Steering);
	W.F32(P.FuelLiters);
	W.F32(P.ErsCharge);
	W.Four(P.TyreTempC);
	W.Four(P.TyrePressureKpa);
	W.Four(P.TyreWearPct);

	// Session.
	W.I32(P.Place);
	W.I32(P.CarCount);
	W.I32(P.Lap);
	W.I32(P.LapLimit);
	W.F32(P.LapTimeS);
	W.F32(P.LastLapS);
	W.F32(P.BestLapS);
	W.F32(P.DeltaS);
	W.F32(P.StationM);
	W.F32(P.RaceLeftS);

	check(Bytes.Num() == PacketBytes);
	return Bytes;
}

void ApexTelemetryOutput::FillFromHud(const FApexHudData& Hud, FPacket& Out)
{
	if (!TelemetryBool(Hud, TEXT("car.present")))
	{
		Out.Flags &= ~CarPresent;
		return;
	}
	Out.Flags |= CarPresent;

	auto Flag = [&Out](uint32 Bit, bool bOn)
	{
		Out.Flags = bOn ? (Out.Flags | Bit) : (Out.Flags & ~Bit);
	};
	Flag(Spectating, TelemetryBool(Hud, TEXT("spectate.active")));
	Flag(Replay, TelemetryBool(Hud, TEXT("replay.active")));
	Flag(OnTrack, TelemetryBool(Hud, TEXT("car.on_track")));
	Flag(InPitLane, TelemetryBool(Hud, TEXT("pit.in_lane")));
	Flag(InGarage, TelemetryBool(Hud, TEXT("car.in_garage")));
	Flag(DrsAllowed, TelemetryBool(Hud, TEXT("car.drs_allowed")));
	Flag(DrsOpen, TelemetryBool(Hud, TEXT("car.drs_open")));
	Flag(Headlights, TelemetryBool(Hud, TEXT("car.headlights")));
	Flag(YellowFlag, TelemetryNumber(Hud, TEXT("flag.yellow"), 0.0f) > 0.0f);
	Flag(BlueFlag, TelemetryBool(Hud, TEXT("flag.blue")));
	Flag(LapInvalid, TelemetryBool(Hud, TEXT("lap.invalid")));
	Flag(Finished, TelemetryNumber(Hud, TEXT("car.finish_position"), 0.0f) > 0.0f);

	Out.SpeedMps = TelemetryNumber(Hud, TEXT("car.speed_kph"), 0.0f) / 3.6f;
	Out.Rpm = TelemetryNumber(Hud, TEXT("car.rpm"), 0.0f);
	Out.RedlineRpm = TelemetryNumber(Hud, TEXT("car.redline_rpm"), 0.0f);
	Out.MaxRpm = TelemetryNumber(Hud, TEXT("car.rpm_max"), 0.0f);
	Out.RevFraction = TelemetryNumber(Hud, TEXT("car.rpm_fraction"), 0.0f);
	Out.Gear = FMath::RoundToInt(TelemetryNumber(Hud, TEXT("car.gear"), 0.0f));
	Out.Throttle = TelemetryNumber(Hud, TEXT("car.throttle"), 0.0f);
	Out.Brake = TelemetryNumber(Hud, TEXT("car.brake"), 0.0f);
	Out.Steering = TelemetryNumber(Hud, TEXT("car.steering"), 0.0f);
	Out.FuelLiters = TelemetryNumber(Hud, TEXT("fuel.liters"), -1.0f);
	const float Ers = TelemetryNumber(Hud, TEXT("ers.charge_pct"), -1.0f);
	Out.ErsCharge = Ers >= 0.0f ? Ers / 100.0f : -1.0f;

	if (const TArray<FApexHudRecord>* Tyres = Hud.FindList(TEXT("tyres")))
	{
		for (int32 Tyre = 0; Tyre < 4 && Tyre < Tyres->Num(); ++Tyre)
		{
			const FApexHudRecord& Row = (*Tyres)[Tyre];
			auto Field = [&Row](const TCHAR* Name)
			{
				const FApexHudValue* Value = Row.Find(FName(Name));
				return Value && !Value->IsNone() ? static_cast<float>(Value->AsNumber()) : -1.0f;
			};
			Out.TyreTempC[Tyre] = Field(TEXT("temp_c"));
			Out.TyrePressureKpa[Tyre] = Field(TEXT("pressure_kpa"));
			Out.TyreWearPct[Tyre] = Field(TEXT("wear_pct"));
		}
	}

	Out.Place = FMath::RoundToInt(TelemetryNumber(Hud, TEXT("race.position"), 0.0f));
	Out.CarCount = FMath::RoundToInt(TelemetryNumber(Hud, TEXT("race.car_count"), 0.0f));
	Out.Lap = FMath::RoundToInt(TelemetryNumber(Hud, TEXT("lap.current"), 0.0f));
	Out.LapLimit = FMath::Max(0, FMath::RoundToInt(TelemetryNumber(Hud, TEXT("lap.limit"), 0.0f)));
	Out.LapTimeS = TelemetryNumber(Hud, TEXT("lap.time_s"), 0.0f);
	Out.LastLapS = TelemetryNumber(Hud, TEXT("lap.last_s"), 0.0f);
	Out.BestLapS = TelemetryNumber(Hud, TEXT("lap.best_s"), 0.0f);
	Out.DeltaS = TelemetryNumber(Hud, TEXT("timing.delta_s"), 0.0f);
	Out.StationM = TelemetryNumber(Hud, TEXT("car.station_m"), 0.0f);
	Out.RaceLeftS = TelemetryNumber(Hud, TEXT("race.time_left_s"), -1.0f);
}

// --- Motion ------------------------------------------------------------------

void ApexTelemetryOutput::CarAxes(const FVector3f& Attitude, FVector3f& OutForward, FVector3f& OutLeft, FVector3f& OutUp)
{
	const float SinYaw = FMath::Sin(Attitude.X);
	const float CosYaw = FMath::Cos(Attitude.X);
	const float SinPitch = FMath::Sin(Attitude.Y);
	const float CosPitch = FMath::Cos(Attitude.Y);
	const float SinRoll = FMath::Sin(Attitude.Z);
	const float CosRoll = FMath::Cos(Attitude.Z);

	// Nose up lifts forward; the level left axis stays level under pitch.
	OutForward = FVector3f(CosPitch * CosYaw, CosPitch * SinYaw, SinPitch);
	const FVector3f LevelLeft(-SinYaw, CosYaw, 0.0f);
	const FVector3f PitchedUp = FVector3f::CrossProduct(OutForward, LevelLeft);
	// Leaning right (positive roll) tips the roof toward the right and lifts
	// the left side.
	OutLeft = LevelLeft * CosRoll + PitchedUp * SinRoll;
	OutUp = PitchedUp * CosRoll - LevelLeft * SinRoll;
}

void ApexTelemetryOutput::FMotionEstimator::Reset()
{
	const float Keep = FilterHz;
	*this = FMotionEstimator();
	FilterHz = Keep;
}

void ApexTelemetryOutput::FMotionEstimator::AddSample(
	int64 ServerTick, double ArrivalSeconds, const FVector3f& Position, const FVector3f& Attitude)
{
	if (Samples > 0)
	{
		if (ServerTick == LastTick)
		{
			return;
		}
		if (ServerTick < LastTick || FVector3f::Dist(Position, LastPosition) > 50.0f)
		{
			Reset();
		}
	}
	if (Samples == 0)
	{
		Samples = 1;
		LastTick = ServerTick;
		LastArrival = ArrivalSeconds;
		LastPosition = Position;
		LastAttitude = Attitude;
		return;
	}

	const int64 Ticks = ServerTick - LastTick;
	// The tick's length from how fast ticks arrive: a step of under a second
	// only, so a pause (a menu, a stall) teaches it nothing. Speed scales with
	// it, so every wobble in it would read as surge: it is learnt as a plain
	// mean over the first second and then held, and learnt again only when
	// the arrivals stay 5% off it (another server's rate).
	const double Arrival = ArrivalSeconds - LastArrival;
	if (Arrival > 0.0 && Arrival < 1.0)
	{
		const double Measured = FMath::Clamp(Arrival / static_cast<double>(Ticks), 1.0 / 2000.0, 1.0 / 20.0);
		const double Before = TickSeconds;
		if (TickMeasurements < TickLearnSamples)
		{
			TickSeconds = TickMeasurements == 0 ? Measured
				: TickSeconds + (Measured - TickSeconds) / (TickMeasurements + 1);
			DriftSeconds = TickSeconds;
		}
		else
		{
			DriftSeconds += (Measured - DriftSeconds) * 0.01;
			if (FMath::Abs(DriftSeconds / TickSeconds - 1.0) > 0.05)
			{
				TickSeconds = DriftSeconds;
			}
		}
		++TickMeasurements;
		// The velocity kept from the last step was measured on the old length.
		WorldVelocity *= static_cast<float>(Before / TickSeconds);
	}
	const float Dt = static_cast<float>(TickSeconds * static_cast<double>(Ticks));
	if (Dt <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const FVector3f Velocity = (Position - LastPosition) / Dt;
	const FVector3f RawRates(
		TelemetryAngleStep(LastAttitude.Z, Attitude.Z) / Dt,
		TelemetryAngleStep(LastAttitude.Y, Attitude.Y) / Dt,
		TelemetryAngleStep(LastAttitude.X, Attitude.X) / Dt);
	// One-pole low pass at FilterHz over this step.
	const float Alpha = 1.0f - FMath::Exp(-2.0f * PI * FMath::Max(FilterHz, 0.1f) * Dt);
	if (Samples >= 2)
	{
		const FVector3f RawAccel = (Velocity - WorldVelocity) / Dt;
		WorldAccel += (RawAccel - WorldAccel) * Alpha;
		Rates += (RawRates - Rates) * Alpha;
	}
	else
	{
		Rates = RawRates;
	}
	WorldVelocity = Velocity;
	LastTick = ServerTick;
	LastArrival = ArrivalSeconds;
	LastPosition = Position;
	LastAttitude = Attitude;
	Samples = FMath::Min(Samples + 1, 3);
}

void ApexTelemetryOutput::FMotionEstimator::Fill(FPacket& Out) const
{
	Out.Position = LastPosition;
	Out.Attitude = LastAttitude;
	if (!HasMotion())
	{
		Out.Flags &= ~MotionValid;
		Out.LocalVelocity = FVector3f::ZeroVector;
		Out.LocalAccel = FVector3f::ZeroVector;
		Out.AngularRate = FVector3f::ZeroVector;
		return;
	}
	Out.Flags |= MotionValid;
	FVector3f Forward, Left, Up;
	CarAxes(LastAttitude, Forward, Left, Up);
	Out.LocalVelocity = FVector3f(
		FVector3f::DotProduct(WorldVelocity, Forward),
		FVector3f::DotProduct(WorldVelocity, Left),
		FVector3f::DotProduct(WorldVelocity, Up));
	Out.LocalAccel = FVector3f(
		FVector3f::DotProduct(WorldAccel, Forward),
		FVector3f::DotProduct(WorldAccel, Left),
		FVector3f::DotProduct(WorldAccel, Up));
	Out.AngularRate = Rates;
}

// --- Subsystem ---------------------------------------------------------------

namespace
{
	/** The menus' heartbeat: an app sees the game is up without a car. */
	constexpr double TelemetryIdleIntervalS = 0.5;

	FAutoConsoleCommandWithWorldAndArgs TelemetryOutCommand(
		TEXT("apexsim.telemetry.Out"),
		TEXT("The telemetry output for external apps (settings.yml telemetry_out): ")
		TEXT("apexsim.telemetry.Out [on|off] switches it for this run; no argument prints where it sends"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			UApexTelemetryOutputSubsystem* Out = GameInstance ? GameInstance->GetSubsystem<UApexTelemetryOutputSubsystem>() : nullptr;
			if (!Out)
			{
				return;
			}
			FApexTelemetryOutputSettings Settings = Out->GetSettings();
			if (Args.Num() > 0)
			{
				Settings.bEnabled = Args[0].Equals(TEXT("on"), ESearchCase::IgnoreCase) || Args[0] == TEXT("1");
				Out->Configure(Settings);
			}
			UE_LOG(LogApexSim, Display, TEXT("Telemetry output %s: %s:%d at %d Hz, %u packet(s) sent"),
				Settings.bEnabled ? TEXT("on") : TEXT("off"), *Settings.Host, Settings.Port, Settings.RateHz,
				Out->GetPacketsSent());
		}));
}

UApexTelemetryOutputSubsystem::UApexTelemetryOutputSubsystem() = default;
UApexTelemetryOutputSubsystem::~UApexTelemetryOutputSubsystem() = default;

void UApexTelemetryOutputSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	Collection.InitializeDependency<UApexHudDataSubsystem>();
	UApexBootSettingsSubsystem* Boot = Collection.InitializeDependency<UApexBootSettingsSubsystem>();

	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnTelemetry.AddDynamic(this, &UApexTelemetryOutputSubsystem::HandleTelemetry);
	}
	StartSeconds = FPlatformTime::Seconds();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UApexTelemetryOutputSubsystem::Tick), 0.0f);
	if (Boot)
	{
		Configure(Boot->Get().TelemetryOut);
	}
}

void UApexTelemetryOutputSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>())
	{
		Net->OnTelemetry.RemoveDynamic(this, &UApexTelemetryOutputSubsystem::HandleTelemetry);
	}
	CloseSocket();
	Super::Deinitialize();
}

void UApexTelemetryOutputSubsystem::Configure(const FApexTelemetryOutputSettings& InSettings)
{
	Settings = InSettings;
	Settings.RateHz = FMath::Clamp(Settings.RateHz, 1, 400);
	CloseSocket();
	if (Settings.bEnabled)
	{
		OpenSocket();
	}
}

void UApexTelemetryOutputSubsystem::OpenSocket()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		return;
	}
	bool bValid = false;
	Destination = Sockets->CreateInternetAddr();
	Destination->SetIp(*Settings.Host, bValid);
	if (!bValid)
	{
		// A name rather than an address: resolve it once, now.
		FAddressInfoResult Info = Sockets->GetAddressInfo(*Settings.Host, nullptr,
			EAddressInfoFlags::Default, NAME_None, ESocketType::SOCKTYPE_Datagram);
		if (Info.ReturnCode == SE_NO_ERROR && Info.Results.Num() > 0)
		{
			Destination = Info.Results[0].Address->Clone();
			bValid = true;
		}
	}
	if (!bValid)
	{
		UE_LOG(LogApexSim, Warning, TEXT("Telemetry output: cannot resolve '%s'; not sending"), *Settings.Host);
		Destination.Reset();
		return;
	}
	Destination->SetPort(Settings.Port);
	Socket = FUdpSocketBuilder(TEXT("ApexTelemetryOut")).AsNonBlocking().WithSendBufferSize(64 * 1024).Build();
	bWarnedSend = false;
	UE_LOG(LogApexSim, Log, TEXT("Telemetry output to %s at %d Hz (protocol %d, %d bytes)"),
		*Destination->ToString(true), Settings.RateHz, ApexTelemetryOutput::Version, ApexTelemetryOutput::PacketBytes);
}

void UApexTelemetryOutputSubsystem::CloseSocket()
{
	if (Socket)
	{
		Socket->Close();
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Sockets->DestroySocket(Socket);
		}
		Socket = nullptr;
	}
	Destination.Reset();
}

void UApexTelemetryOutputSubsystem::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	if (!Socket)
	{
		return;
	}
	const UApexHudDataSubsystem* Hud = GetGameInstance()->GetSubsystem<UApexHudDataSubsystem>();
	const FApexHudValue* Index = Hud ? Hud->GetData().Find(TEXT("car.index")) : nullptr;
	const UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	// The HUD's car (the player's, or the one watched); the player's own
	// before the HUD has been built.
	const int32 CarIndex = Index && !Index->IsNone() ? FMath::RoundToInt(Index->AsNumber())
		: Net ? Net->GetLocalCarIndex() : -1;
	if (CarIndex != MotionCarIndex)
	{
		Motion.Reset();
		MotionCarIndex = CarIndex;
	}
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		if (Car.CarIndex == CarIndex)
		{
			Motion.AddSample(Frame.ServerTick, FPlatformTime::Seconds(), FVector3f(Car.Position),
				FVector3f(Car.YawRad, Car.PitchRad, Car.RollRad));
			return;
		}
	}
}

bool UApexTelemetryOutputSubsystem::Tick(float DeltaSeconds)
{
	if (!Socket)
	{
		return true;
	}
	SinceSend += DeltaSeconds;
	UApexHudDataSubsystem* Hud = GetGameInstance()->GetSubsystem<UApexHudDataSubsystem>();
	const bool bRace = Hud && Hud->IsRaceActive();
	const double Interval = bRace ? 1.0 / Settings.RateHz : TelemetryIdleIntervalS;
	if (SinceSend < Interval)
	{
		return true;
	}
	// Catch up by whole intervals, never in a burst.
	SinceSend = FMath::Fmod(SinceSend, Interval);
	Send();
	return true;
}

void UApexTelemetryOutputSubsystem::Send()
{
	using namespace ApexTelemetryOutput;

	FPacket Packet;
	Packet.Sequence = Sequence;
	Packet.TimeS = static_cast<float>(FPlatformTime::Seconds() - StartSeconds);

	UApexHudDataSubsystem* Hud = GetGameInstance()->GetSubsystem<UApexHudDataSubsystem>();
	const UApexNetSubsystem* Net = GetGameInstance()->GetSubsystem<UApexNetSubsystem>();
	if (Hud && Hud->IsRaceActive())
	{
		// The HUD builds its data each frame while it is drawn; with the HUD
		// hidden nothing else would.
		Hud->RefreshIfStale();
		FillFromHud(Hud->GetData(), Packet);
		if (Packet.Flags & CarPresent)
		{
			Motion.Fill(Packet);
			if (Net && !(Packet.Flags & Spectating))
			{
				const FApexDriverFeedback& Feedback = Net->GetDriverFeedback();
				// The feedback says what happened over its interval; a stale one
				// (a pause, a pit stop's hold) says nothing now.
				const bool bFresh = FPlatformTime::Seconds() - Net->GetDriverFeedbackTime() < 0.25;
				if (bFresh && Feedback.bAbsActive)
				{
					Packet.Flags |= AbsActive;
				}
				if (bFresh && Feedback.bTcActive)
				{
					Packet.Flags |= TcActive;
				}
			}
		}
	}
	if (Net)
	{
		Packet.GameMode = static_cast<int32>(Net->GetGameMode());
	}

	const TArray<uint8> Bytes = Encode(Packet);
	int32 Sent = 0;
	if (!Socket->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *Destination) && !bWarnedSend)
	{
		bWarnedSend = true;
		UE_LOG(LogApexSim, Warning, TEXT("Telemetry output: send to %s failed (logged once)"), *Destination->ToString(true));
	}
	++Sequence;
}
