#include "Misc/AutomationTest.h"

#include "ApexProtocolCodec.h"
#include "ApexSpectatorStream.h"
#include "Tests/ApexSpectatorGoldenBlobs.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexSpectatorTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

	TArrayView<const uint8> View(const uint8* Data, int32 Num)
	{
		return TArrayView<const uint8>(Data, Num);
	}

	/** A copy of the golden frame with its epoch, revision, part and parts changed. */
	TArray<uint8> FrameWith(uint32 Epoch, uint8 Revision, uint8 Part, uint8 Parts)
	{
		TArray<uint8> Body(ApexSpectatorGolden::Frame, UE_ARRAY_COUNT(ApexSpectatorGolden::Frame));
		// [0x99, 3, CE e e e e, CE t t t t, rev, state, CD c c, part, parts, bin...]
		Body[3] = static_cast<uint8>(Epoch >> 24);
		Body[4] = static_cast<uint8>(Epoch >> 16);
		Body[5] = static_cast<uint8>(Epoch >> 8);
		Body[6] = static_cast<uint8>(Epoch);
		Body[12] = Revision;
		Body[17] = Part;
		Body[18] = Parts;
		return Body;
	}

	TArray<uint8> TickOf(const TArray<uint8>& Body, uint32 Tick)
	{
		TArray<uint8> Out = Body;
		Out[8] = static_cast<uint8>(Tick >> 24);
		Out[9] = static_cast<uint8>(Tick >> 16);
		Out[10] = static_cast<uint8>(Tick >> 8);
		Out[11] = static_cast<uint8>(Tick);
		return Out;
	}
}

// -----------------------------------------------------------------------------
// Codec: every record decodes from the server's golden bytes.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectatorCodecTest, "ApexSim.Spectator.Codec", ApexSpectatorTests::Flags)

bool FApexSpectatorCodecTest::RunTest(const FString& Parameters)
{
	using namespace ApexSpectatorGolden;
	using namespace ApexSpectatorTests;
	FApexStreamRecord Record;
	FString Error;

	// Header.
	TestEqual(TEXT("header type"), (int32)ApexSpectator::RecordType(View(Header, UE_ARRAY_COUNT(Header))), (int32)ApexSpectator::RecordHeader);
	uint32 Epoch = 0;
	TestTrue(TEXT("header epoch read in place"), ApexSpectator::RecordEpoch(View(Header, UE_ARRAY_COUNT(Header)), Epoch) && Epoch == 3);
	TestTrue(TEXT("header decodes"), ApexSpectator::DecodeRecord(View(Header, UE_ARRAY_COUNT(Header)), Record, Error));
	TestEqual(TEXT("header epoch"), (int32)Record.Header.Epoch, 3);
	TestEqual(TEXT("header stream id"), Record.Header.StreamId, FString(TEXT("01234567-89ab-cdef-0123-456789abcdef")));
	TestEqual(TEXT("header tick rate"), Record.Header.TickRate, 240);
	TestEqual(TEXT("header frame rate"), Record.Header.FrameRate, 30);
	TestEqual(TEXT("header row size"), Record.Header.RowSize, 44);
	TestEqual(TEXT("header stem"), Record.Header.Track.Stem, FString(TEXT("Zandvoort")));
	TestEqual(TEXT("header display name"), Record.Header.Track.DisplayName, FString(TEXT("Zandervoort")));
	TestTrue(TEXT("header crc"), Record.Header.Track.SourceCrc == 0xCBF43926u);
	TestTrue(TEXT("header length"), FMath::IsNearlyEqual(Record.Header.Track.LengthM, 4259.0f));
	TestEqual(TEXT("header weather"), (int32)Record.Header.Conditions.Weather, (int32)EApexWeather::LightRain);
	TestEqual(TEXT("header clock"), Record.Header.Conditions.TimeOfDayMinutes, 21 * 60 + 30);
	TestEqual(TEXT("header air"), Record.Header.Conditions.AirTempC, -3);
	TestEqual(TEXT("header humidity"), Record.Header.Conditions.HumidityPct, 85);
	TestEqual(TEXT("header wind"), Record.Header.Conditions.WindKph, 22);
	TestEqual(TEXT("header wind from"), Record.Header.Conditions.WindFromDeg, 270);
	TestEqual(TEXT("header mode"), (int32)Record.Header.GameMode, (int32)EApexGameMode::Race);
	TestEqual(TEXT("header laps"), Record.Header.LapLimit, 2);
	TestEqual(TEXT("header race start"), Record.Header.RaceStartTick, (int64)1920);
	TestEqual(TEXT("header start"), Record.Header.StartTick, (int64)8);
	TestEqual(TEXT("header end"), Record.Header.EndTick, (int64)52800);
	TestEqual(TEXT("header seed"), Record.Header.Seed, (int64)7);
	TestTrue(TEXT("header score"), Record.Header.bHasScore && FMath::IsNearlyEqual(Record.Header.Score, 123.5f));
	TestTrue(TEXT("header duration"), FMath::IsNearlyEqual(Record.Header.DurationSeconds(), 219.9666, 0.01));

	// Roster.
	TestTrue(TEXT("roster decodes"), ApexSpectator::DecodeRecord(View(Roster, UE_ARRAY_COUNT(Roster)), Record, Error));
	TestEqual(TEXT("roster revision"), Record.Roster.Revision, 1);
	TestEqual(TEXT("roster entries"), Record.Roster.Entries.Num(), 2);
	if (Record.Roster.Entries.Num() == 2)
	{
		TestEqual(TEXT("roster car id"), Record.Roster.Entries[0].CarConfigId, FString(TEXT("11111111-2222-3333-4444-555555555555")));
		TestTrue(TEXT("roster crc"), Record.Roster.Entries[0].ContentCrc == 0x12345678u);
		TestEqual(TEXT("roster livery"), Record.Roster.Entries[0].Livery, 2);
		TestEqual(TEXT("roster name"), Record.Roster.Entries[0].Name, FString(TEXT("A. Driver")));
		TestTrue(TEXT("roster ai"), Record.Roster.Entries[0].bIsAi && !Record.Roster.Entries[1].bIsAi);
		const FApexSessionRoster Session = Record.Roster.ToSessionRoster(TEXT("s"));
		TestEqual(TEXT("session roster"), Session.Entries.Num(), 2);
		TestEqual(TEXT("session roster index"), Session.Entries[1].CarIndex, 1);
		TestEqual(TEXT("session roster car"), Session.Entries[1].CarConfigId, FString(TEXT("66666666-7777-8888-9999-aaaaaaaaaaaa")));
	}

	// Frame and its rows.
	int64 Tick = 0;
	TestTrue(TEXT("frame tick read in place"), ApexSpectator::RecordTick(View(Frame, UE_ARRAY_COUNT(Frame)), Tick) && Tick == 4808);
	TestTrue(TEXT("frame decodes"), ApexSpectator::DecodeRecord(View(Frame, UE_ARRAY_COUNT(Frame)), Record, Error));
	TestEqual(TEXT("frame epoch"), (int32)Record.Frame.Epoch, 3);
	TestEqual(TEXT("frame tick"), Record.Frame.Tick, (int64)4808);
	TestEqual(TEXT("frame revision"), Record.Frame.RosterRevision, 1);
	TestEqual(TEXT("frame state"), (int32)Record.Frame.State, (int32)EApexSessionState::Racing);
	TestEqual(TEXT("frame countdown"), Record.Frame.CountdownMs, 1500);
	TestEqual(TEXT("frame parts"), Record.Frame.Parts, 1);
	TestEqual(TEXT("frame rows bytes"), Record.Frame.Rows.Num(), 88);
	const TArray<FApexStreamCarRow> Rows = Record.Frame.Cars(44);
	TestEqual(TEXT("frame rows"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		const FApexStreamCarRow& Row = Rows[1];
		TestEqual(TEXT("row index"), Row.CarIndex, 1);
		TestEqual(TEXT("row x"), Row.XMm, 100500);
		TestEqual(TEXT("row y"), Row.YMm, -20250);
		TestEqual(TEXT("row z"), Row.ZMm, 500);
		TestEqual(TEXT("row speed"), (int32)Row.SpeedCms, 4200);
		TestEqual(TEXT("row steering"), (int32)Row.Steering, -64);
		TestEqual(TEXT("row throttle"), (int32)Row.Throttle, 255);
		TestEqual(TEXT("row gear"), (int32)Row.Gear, 4);
		TestEqual(TEXT("row rpm"), (int32)Row.EngineRpm, 11000);
		TestEqual(TEXT("row lap"), (int32)Row.Lap, 3);
		TestEqual(TEXT("row station"), (int32)Row.StationCm, 123456);
		TestEqual(TEXT("row lap flags"), (int32)Row.LapFlags, 0x29);
		TestEqual(TEXT("row pit flags"), (int32)Row.PitFlags, 5);
		TestEqual(TEXT("row compound"), (int32)Row.Compound, 1);
		TestEqual(TEXT("row damage"), (int32)Row.Damage[4], 40);
		TestEqual(TEXT("row ers"), (int32)Row.ErsFlags, 5);

		const FApexCarTelemetry T = Row.ToTelemetry();
		TestEqual(TEXT("telemetry index"), T.CarIndex, 1);
		TestTrue(TEXT("telemetry position"), T.Position.Equals(FVector(100.5, -20.25, 0.5), 1e-3));
		TestTrue(TEXT("telemetry yaw"), FMath::IsNearlyEqual(T.YawRad, 1.5f, 1e-3f));
		TestTrue(TEXT("telemetry roll"), FMath::IsNearlyEqual(T.RollRad, -0.25f, 1e-3f));
		TestTrue(TEXT("telemetry speed"), FMath::IsNearlyEqual(T.SpeedMps, 42.0f));
		TestTrue(TEXT("telemetry steering"), FMath::IsNearlyEqual(T.Steering, -64.0f / 127.0f));
		TestTrue(TEXT("telemetry station"), FMath::IsNearlyEqual(T.TrackProgress, 1234.56f, 0.01f));
		TestTrue(TEXT("telemetry status"), T.bIsOnTrack && T.bIsColliding && !T.bInGarage);
		TestTrue(TEXT("telemetry lap flags"), T.bLapInvalid && !T.bLastLapInvalid && T.bDrsAllowed && !T.bDrsOpen && T.bHeadlights);
		TestTrue(TEXT("telemetry pit flags"), T.bPitLimiter && !T.bPitServicing && T.bInPitLane);
		TestEqual(TEXT("telemetry compound"), T.Compound, 1);
		TestTrue(TEXT("telemetry damage"), T.HasDamage() && FMath::IsNearlyEqual(T.DamagePct[0], 12.0f) && FMath::IsNearlyEqual(T.DamagePct[4], 40.0f));
		TestTrue(TEXT("telemetry ers"), T.ErsMode == 1 && T.bErsDeploying && !T.bErsHarvesting && !T.bErsBoost);
		TestTrue(TEXT("telemetry unknowns"), !T.HasTyres() && T.FuelLiters < 0.0f && !T.HasHybrid());

		// Written back, the row is its 44 bytes.
		TArray<uint8> Back;
		Row.Write(Back);
		TestTrue(TEXT("row round trip"), Back == TArray<uint8>(Record.Frame.Rows.GetData() + 44, 44));
	}

	// A newer writer's longer row: the known fields read, the rest ignored.
	{
		TArray<uint8> Longer = Record.Frame.Rows;
		Longer.Insert(TArray<uint8>{9, 9, 9, 9}, 44);
		Longer.Append({ 9, 9, 9, 9 });
		FApexStreamFrame Wide = Record.Frame;
		Wide.Rows = Longer;
		const TArray<FApexStreamCarRow> WideRows = Wide.Cars(48);
		TestEqual(TEXT("wide rows"), WideRows.Num(), 2);
		TestTrue(TEXT("wide rows read the known fields"), WideRows.Num() == 2 && WideRows[1].XMm == 100500 && WideRows[1].ErsFlags == 5);
	}

	// Events.
	TestTrue(TEXT("lap timing decodes"), ApexSpectator::DecodeRecord(View(EventLapTiming, UE_ARRAY_COUNT(EventLapTiming)), Record, Error));
	TestEqual(TEXT("lap timing kind"), (int32)Record.Event.Kind, (int32)ApexSpectator::EventLapTiming);
	TestEqual(TEXT("lap timing tick"), Record.Event.Tick, (int64)9000);
	TestEqual(TEXT("lap timing car"), Record.Event.LapTiming.CarIndex, 2);
	TestEqual(TEXT("lap timing lap"), Record.Event.LapTiming.Lap, 3);
	TestEqual(TEXT("lap timing sector"), Record.Event.LapTiming.Sector, 2);
	TestEqual(TEXT("lap timing sector ms"), Record.Event.LapTiming.SectorTimeMs, 28114);
	TestEqual(TEXT("lap timing lap ms"), Record.Event.LapTiming.LapTimeMs, 82615);
	TestTrue(TEXT("lap timing flags"), Record.Event.LapTiming.bIsLapEnd && Record.Event.LapTiming.bValid
		&& Record.Event.LapTiming.bPersonalBestLap && !Record.Event.LapTiming.bSessionBestLap && Record.Event.LapTiming.bPersonalBestSector);

	TestTrue(TEXT("sectors decode"), ApexSpectator::DecodeRecord(View(EventTrackSectors, UE_ARRAY_COUNT(EventTrackSectors)), Record, Error));
	TestEqual(TEXT("sectors kind"), (int32)Record.Event.Kind, (int32)ApexSpectator::EventTrackSectors);
	TestTrue(TEXT("sectors length"), FMath::IsNearlyEqual(Record.Event.Sectors.TrackLengthM, 4259.0f));
	TestEqual(TEXT("sectors boundaries"), Record.Event.Sectors.BoundariesM.Num(), 2);
	TestTrue(TEXT("sectors boundary"), Record.Event.Sectors.BoundariesM.Num() == 2 && FMath::IsNearlyEqual(Record.Event.Sectors.BoundariesM[1], 2900.0f));

	TestTrue(TEXT("contact decodes"), ApexSpectator::DecodeRecord(View(EventContact, UE_ARRAY_COUNT(EventContact)), Record, Error));
	TestEqual(TEXT("contact kind"), (int32)Record.Event.Kind, (int32)ApexSpectator::EventContact);
	TestEqual(TEXT("contact car"), Record.Event.CarIndex, 7);
	TestEqual(TEXT("contact other"), Record.Event.Other, 255);
	TestEqual(TEXT("contact speed"), Record.Event.SpeedCms, 5100);

	// Path.
	TestTrue(TEXT("path decodes"), ApexSpectator::DecodeRecord(View(Path, UE_ARRAY_COUNT(Path)), Record, Error));
	TestEqual(TEXT("path points"), Record.Path.Points.Num(), 3);
	TestTrue(TEXT("path spacing"), FMath::IsNearlyEqual(Record.Path.SpacingM, 10.0f));
	TestTrue(TEXT("path point"), Record.Path.Points.Num() == 3 && Record.Path.Points[2].Equals(FVector2D(-19.75, 3.25), 1e-3));

	// Rubbish is refused; an unknown record type is reported, not refused.
	const uint8 Rubbish[] = { 0xC0, 0x01 };
	TestFalse(TEXT("rubbish refused"), ApexSpectator::DecodeRecord(View(Rubbish, 2), Record, Error));
	const uint8 Unknown[] = { 0x92, 0x2A, 0xC0 };
	TestTrue(TEXT("unknown type decodes"), ApexSpectator::DecodeRecord(View(Unknown, 3), Record, Error) && Record.Type == 42);
	return true;
}

// -----------------------------------------------------------------------------
// File: the preamble, the index, a block inflated, a seek by tick.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectatorFileTest, "ApexSim.Spectator.File", ApexSpectatorTests::Flags)

bool FApexSpectatorFileTest::RunTest(const FString& Parameters)
{
	using namespace ApexSpectatorGolden;
	FApexStreamFile StreamFile;
	FString Error;
	TestTrue(TEXT("file loads"), StreamFile.LoadFromBytes(TArray<uint8>(File, UE_ARRAY_COUNT(File)), Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("file header stem"), StreamFile.GetHeader().Track.Stem, FString(TEXT("Zandvoort")));
	TestEqual(TEXT("file header epoch"), (int32)StreamFile.GetHeader().Epoch, 0);
	TestEqual(TEXT("file start"), StreamFile.GetHeader().StartTick, (int64)8);
	TestEqual(TEXT("file end"), StreamFile.GetHeader().EndTick, (int64)8 + 89 * 8);
	TestEqual(TEXT("file roster"), StreamFile.GetRoster().Entries.Num(), 2);
	TestTrue(TEXT("file path"), StreamFile.HasPath() && StreamFile.GetPath().Points.Num() == 2);
	TestEqual(TEXT("file preamble"), StreamFile.GetPreamble().Num(), 1);
	TestTrue(TEXT("file preamble sectors"), StreamFile.GetPreamble().Num() == 1 && StreamFile.GetPreamble()[0].Kind == ApexSpectator::EventTrackSectors);
	TestEqual(TEXT("file blocks"), StreamFile.NumBlocks(), 3);
	TestEqual(TEXT("file block 1 tick"), StreamFile.GetIndex()[1].FirstTick, (int64)248);
	TestEqual(TEXT("file block for tick"), StreamFile.BlockForTick(250), 1);
	TestEqual(TEXT("file block for a late tick"), StreamFile.BlockForTick(100000), 2);
	TestEqual(TEXT("file block for an early tick"), StreamFile.BlockForTick(0), 0);

	TArray<TArray<uint8>> Bodies;
	TestTrue(TEXT("block inflates"), StreamFile.ReadBlock(1, Bodies, Error));
	int64 Tick = 0;
	TestTrue(TEXT("block first tick"), Bodies.Num() > 0 && ApexSpectator::RecordTick(Bodies[0], Tick) && Tick == 248);
	bool bEvent = false;
	for (const TArray<uint8>& Body : Bodies)
	{
		bEvent |= ApexSpectator::RecordType(Body) == ApexSpectator::RecordEvent;
	}
	TestTrue(TEXT("block holds the finish event"), bEvent);

	TArray<TArray<uint8>> All;
	TestTrue(TEXT("whole file inflates"), StreamFile.ReadAll(All, Error));
	TestEqual(TEXT("whole file records"), All.Num(), 91);

	// The preamble bytes replay through a player.
	FApexSpectatorPlayer Player;
	TestTrue(TEXT("preamble applies"), Player.ApplyFramed(StreamFile.GetPreambleBytes(), Error));
	TestTrue(TEXT("preamble gives header and roster"), Player.HasHeader() && Player.HasRoster() && Player.HasPath());
	TestEqual(TEXT("preamble events"), Player.TakeEvents().Num(), 1);
	for (const TArray<uint8>& Body : All)
	{
		Player.Apply(Body, Error);
	}
	TestEqual(TEXT("every frame of the file plays"), Player.TakeFrames().Num(), 90);
	TestEqual(TEXT("the finish event plays"), Player.TakeEvents().Num(), 1);
	TestEqual(TEXT("nothing dropped"), Player.GetDroppedFrames(), 0);

	FApexStreamFile Bad;
	TestFalse(TEXT("not a file"), Bad.LoadFromBytes(TArray<uint8>{ 'n', 'o', 'p', 'e', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, Error));
	TestFalse(TEXT("too short"), Bad.LoadFromBytes(TArray<uint8>{ 'A', 'P', 'X', 'S' }, Error));
	return true;
}

// -----------------------------------------------------------------------------
// Player: epochs, revisions and parts.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectatorPlayerTest, "ApexSim.Spectator.Player", ApexSpectatorTests::Flags)

bool FApexSpectatorPlayerTest::RunTest(const FString& Parameters)
{
	using namespace ApexSpectatorGolden;
	using namespace ApexSpectatorTests;
	FApexSpectatorPlayer Player;
	FString Error;

	// A frame before the header or roster is dropped.
	Player.Apply(FrameWith(3, 1, 0, 1), Error);
	TestEqual(TEXT("frame before header dropped"), Player.GetDroppedFrames(), 1);
	TestTrue(TEXT("header applies"), Player.Apply(View(Header, UE_ARRAY_COUNT(Header)), Error));
	TestTrue(TEXT("header changed"), Player.TakeHeaderChanged() && !Player.TakeHeaderChanged());
	Player.Apply(FrameWith(3, 1, 0, 1), Error);
	TestEqual(TEXT("frame before roster dropped"), Player.GetDroppedFrames(), 2);
	TestTrue(TEXT("roster applies"), Player.Apply(View(Roster, UE_ARRAY_COUNT(Roster)), Error));
	TestTrue(TEXT("roster changed"), Player.TakeRosterChanged());

	// The right epoch and revision: a frame of two cars.
	Player.Apply(FrameWith(3, 1, 0, 1), Error);
	TArray<FApexTelemetryFrame> Frames = Player.TakeFrames();
	TestEqual(TEXT("one frame"), Frames.Num(), 1);
	if (Frames.Num() == 1)
	{
		TestEqual(TEXT("frame tick"), Frames[0].ServerTick, (int64)4808);
		TestEqual(TEXT("frame cars"), Frames[0].Cars.Num(), 2);
		TestEqual(TEXT("frame state"), (int32)Frames[0].SessionState, (int32)EApexSessionState::Racing);
		TestEqual(TEXT("frame mode from the header"), (int32)Frames[0].GameMode, (int32)EApexGameMode::Race);
		TestEqual(TEXT("frame countdown"), Frames[0].CountdownMs, 1500);
	}

	// Another epoch, another revision: stale, dropped.
	Player.Apply(FrameWith(4, 1, 0, 1), Error);
	Player.Apply(FrameWith(3, 0, 0, 1), Error);
	TestEqual(TEXT("stale frames dropped"), Player.GetDroppedFrames(), 4);
	TestEqual(TEXT("nothing taken"), Player.TakeFrames().Num(), 0);

	// Parts: two halves of one tick make one frame of four cars (the golden
	// rows twice), in whichever order they arrive, each half once.
	Player.Apply(FrameWith(3, 1, 1, 2), Error);
	TestEqual(TEXT("half a frame is held"), Player.TakeFrames().Num(), 0);
	Player.Apply(FrameWith(3, 1, 1, 2), Error);
	Player.Apply(FrameWith(3, 1, 0, 2), Error);
	Frames = Player.TakeFrames();
	TestEqual(TEXT("two parts, one frame"), Frames.Num(), 1);
	TestTrue(TEXT("both parts' cars"), Frames.Num() == 1 && Frames[0].Cars.Num() == 4);
	TestEqual(TEXT("complete"), Player.GetIncompleteFrames(), 0);

	// A part missing: the next tick flushes what there is.
	Player.Apply(TickOf(FrameWith(3, 1, 0, 2), 5000), Error);
	Player.Apply(TickOf(FrameWith(3, 1, 0, 1), 5008), Error);
	Frames = Player.TakeFrames();
	TestEqual(TEXT("incomplete then whole"), Frames.Num(), 2);
	TestTrue(TEXT("incomplete has its rows"), Frames.Num() == 2 && Frames[0].Cars.Num() == 2 && Frames[1].Cars.Num() == 2);
	TestEqual(TEXT("one incomplete"), Player.GetIncompleteFrames(), 1);

	// Events pass with the epoch; an event of another epoch does not.
	Player.Apply(View(EventLapTiming, UE_ARRAY_COUNT(EventLapTiming)), Error);
	TArray<uint8> Stale(EventLapTiming, UE_ARRAY_COUNT(EventLapTiming));
	Stale[6] = 9;
	Player.Apply(Stale, Error);
	TestEqual(TEXT("one event"), Player.TakeEvents().Num(), 1);

	// A new header is a new epoch: the roster must come again.
	TArray<uint8> NextHeader(Header, UE_ARRAY_COUNT(Header));
	NextHeader[6] = 4;
	Player.Apply(NextHeader, Error);
	TestTrue(TEXT("new epoch"), Player.GetEpoch() == 4 && !Player.HasRoster());
	Player.Apply(FrameWith(4, 1, 0, 1), Error);
	TestEqual(TEXT("no roster yet"), Player.TakeFrames().Num(), 0);
	TArray<uint8> NextRoster(Roster, UE_ARRAY_COUNT(Roster));
	NextRoster[6] = 4;
	Player.Apply(NextRoster, Error);
	Player.Apply(FrameWith(4, 1, 0, 1), Error);
	TestEqual(TEXT("frames again"), Player.TakeFrames().Num(), 1);

	// A framed run (one TCP message) applies in order.
	TArray<uint8> Run;
	ApexSpectator::AppendFramed(Run, NextHeader);
	ApexSpectator::AppendFramed(Run, NextRoster);
	ApexSpectator::AppendFramed(Run, FrameWith(4, 1, 0, 1));
	TestTrue(TEXT("run applies"), Player.ApplyFramed(Run, Error));
	TestEqual(TEXT("run gives a frame"), Player.TakeFrames().Num(), 1);
	TArray<TArrayView<const uint8>> Views;
	TestTrue(TEXT("run splits"), ApexSpectator::SplitFramed(Run, Views, Error) && Views.Num() == 3);
	Run.Pop();
	TestFalse(TEXT("a truncated run is refused"), ApexSpectator::SplitFramed(Run, Views, Error));
	return true;
}

// -----------------------------------------------------------------------------
// Protocol: the showcase messages, and a frame datagram.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSpectatorProtocolTest, "ApexSim.Spectator.Protocol", ApexSpectatorTests::Flags)

bool FApexSpectatorProtocolTest::RunTest(const FString& Parameters)
{
	using namespace ApexShowcaseGolden;
	using namespace ApexSpectatorTests;
	auto Same = [](const TArray<uint8>& Actual, const uint8* Expected, int32 Num)
	{
		return Actual.Num() == Num && FMemory::Memcmp(Actual.GetData(), Expected, Num) == 0;
	};
	TestTrue(TEXT("ListShowcases bytes"), Same(ApexProtocol::EncodeListShowcases(), C_ListShowcases, UE_ARRAY_COUNT(C_ListShowcases)));
	TestTrue(TEXT("SpectateShowcase bytes"), Same(ApexProtocol::EncodeSpectateShowcase(TEXT("Zandvoort.gt3.day")), C_SpectateShowcase, UE_ARRAY_COUNT(C_SpectateShowcase)));
	TestTrue(TEXT("SpectateShowcase (any) bytes"), Same(ApexProtocol::EncodeSpectateShowcase(FString()), C_SpectateShowcaseAny, UE_ARRAY_COUNT(C_SpectateShowcaseAny)));
	TestTrue(TEXT("LeaveSpectate bytes"), Same(ApexProtocol::EncodeLeaveSpectate(), C_LeaveSpectate, UE_ARRAY_COUNT(C_LeaveSpectate)));

	FApexServerMessage Message;
	FString Error;
	TestTrue(TEXT("Showcases decodes"), ApexProtocol::DecodeServerMessage(View(S_Showcases, UE_ARRAY_COUNT(S_Showcases)), Message, Error));
	TestEqual(TEXT("Showcases type"), (int32)Message.Type, (int32)EApexServerMessageType::Showcases);
	TestEqual(TEXT("Showcases entries"), Message.Showcases.Num(), 1);
	if (Message.Showcases.Num() == 1)
	{
		const FApexShowcaseSummary& S = Message.Showcases[0];
		TestEqual(TEXT("showcase id"), S.Id, FString(TEXT("Zandvoort.gt3.day")));
		TestEqual(TEXT("showcase track"), S.TrackId, FString(TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")));
		TestEqual(TEXT("showcase name"), S.TrackName, FString(TEXT("Zandervoort")));
		TestEqual(TEXT("showcase class"), S.Class, FString(TEXT("GT3")));
		TestEqual(TEXT("showcase weather"), (int32)S.Conditions.Weather, (int32)EApexWeather::Cloudy);
		TestEqual(TEXT("showcase clock"), S.Conditions.TimeOfDayMinutes, 16 * 60 + 30);
		TestEqual(TEXT("showcase wind from"), S.Conditions.WindFromDeg, 90);
		TestTrue(TEXT("showcase duration"), FMath::IsNearlyEqual(S.DurationS, 259.5f));
		TestEqual(TEXT("showcase cars"), S.Cars, 16);
		TestEqual(TEXT("showcase viewers"), S.Viewers, 2);
	}
	TestTrue(TEXT("SpectatorJoined decodes"), ApexProtocol::DecodeServerMessage(View(S_SpectatorJoined, UE_ARRAY_COUNT(S_SpectatorJoined)), Message, Error));
	TestEqual(TEXT("SpectatorJoined type"), (int32)Message.Type, (int32)EApexServerMessageType::SpectatorJoined);
	TestEqual(TEXT("SpectatorJoined stream"), Message.StreamId, FString(TEXT("01234567-89ab-cdef-0123-456789abcdef")));
	TestEqual(TEXT("SpectatorJoined kind"), (int32)Message.SpectatorKind, (int32)EApexSpectatorKind::Showcase);
	TestEqual(TEXT("SpectatorJoined id"), Message.ShowcaseId, FString(TEXT("Zandvoort.gt3.day")));

	TestTrue(TEXT("SpectatorRecord decodes"), ApexProtocol::DecodeServerMessage(View(S_SpectatorRecord, UE_ARRAY_COUNT(S_SpectatorRecord)), Message, Error));
	TestEqual(TEXT("SpectatorRecord type"), (int32)Message.Type, (int32)EApexServerMessageType::SpectatorRecord);
	TArray<TArrayView<const uint8>> Bodies;
	TestTrue(TEXT("SpectatorRecord splits"), ApexSpectator::SplitFramed(Message.SpectatorRecords, Bodies, Error) && Bodies.Num() == 2);
	TestTrue(TEXT("SpectatorRecord bodies"), Bodies.Num() == 2 && Bodies[0].Num() == 3 && Bodies[0][1] == 7 && Bodies[1].Num() == 4 && Bodies[1][1] == 8);

	// A frame as a datagram: a bare record body, framed on the way in.
	TestTrue(TEXT("frame datagram decodes"),
		ApexProtocol::DecodeUdpMessage(View(ApexSpectatorGolden::Frame, UE_ARRAY_COUNT(ApexSpectatorGolden::Frame)), Message, Error));
	TestEqual(TEXT("frame datagram type"), (int32)Message.Type, (int32)EApexServerMessageType::SpectatorRecord);
	TestTrue(TEXT("frame datagram framed"), ApexSpectator::SplitFramed(Message.SpectatorRecords, Bodies, Error) && Bodies.Num() == 1
		&& Bodies[0].Num() == UE_ARRAY_COUNT(ApexSpectatorGolden::Frame));

	// A lobby state from a server with showcases says so; the golden one
	// from before the field does not.
	const uint8 Lobby[] = {
		0x82, 0xA4, 't', 'y', 'p', 'e', 0xAA, 'L', 'o', 'b', 'b', 'y', 'S', 't', 'a', 't', 'e',
		0xA4, 'd', 'a', 't', 'a', 0x81, 0xB1, 'S', 'h', 'o', 'w', 'c', 'a', 's', 'e', 'A', 'v', 'a', 'i', 'l', 'a', 'b', 'l', 'e', 0xC3 };
	TestTrue(TEXT("lobby decodes"), ApexProtocol::DecodeServerMessage(View(Lobby, UE_ARRAY_COUNT(Lobby)), Message, Error));
	TestTrue(TEXT("lobby showcase flag"), Message.Type == EApexServerMessageType::LobbyState && Message.LobbyState.bShowcaseAvailable);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
