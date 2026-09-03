#include "Misc/AutomationTest.h"

#include "ApexBootSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags ApexTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexBootSettingsRoundTripTest,
	"ApexSim.Settings.BootSettingsRoundTrip",
	ApexTestFlags)

bool FApexBootSettingsRoundTripTest::RunTest(const FString& Parameters)
{
	// What the game writes, the game has to be able to read back — the file is
	// rewritten on every display change, so a lossy round trip would corrupt a
	// player's settings a little more on each launch.
	FApexBootSettings Written;
	Written.Resolution = FIntPoint(2560, 1440);
	Written.WindowMode = static_cast<int32>(EWindowMode::Windowed);
	Written.bVSync = true;
	Written.FrameLimit = 0;
	Written.ServerHost = TEXT("race.example.net");
	Written.ServerPort = 9100;

	FApexBootSettings Read;
	ApexBootSettingsIo::Parse(ApexBootSettingsIo::Serialise(Written), Read);

	// TestEqual has no FIntPoint overload, so the edges are compared separately.
	TestEqual(TEXT("resolution width survives"), Read.Resolution.X, Written.Resolution.X);
	TestEqual(TEXT("resolution height survives"), Read.Resolution.Y, Written.Resolution.Y);
	TestEqual(TEXT("window mode survives"), Read.WindowMode, Written.WindowMode);
	TestTrue(TEXT("vsync survives"), Read.bVSync);
	TestEqual(TEXT("uncapped frame limit survives"), Read.FrameLimit, 0);
	TestEqual(TEXT("host survives"), Read.ServerHost, Written.ServerHost);
	TestEqual(TEXT("port survives"), Read.ServerPort, Written.ServerPort);

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexBootSettingsParseTest,
	"ApexSim.Settings.BootSettingsParse",
	ApexTestFlags)

bool FApexBootSettingsParseTest::RunTest(const FString& Parameters)
{
	// A hand-written file, not one of ours: odd spacing, a trailing comment, a
	// bool spelled the way a person would, and a section left out entirely.
	const FString Hand =
		TEXT("# mine\n")
		TEXT("display:\n")
		TEXT("    resolution:   1280 x 720   # small on purpose\n")
		TEXT("    window_mode: BORDERLESS\n")
		TEXT("    vsync: yes\n")
		TEXT("\n");

	FApexBootSettings Settings;
	const FString UntouchedHost = Settings.ServerHost;
	const int32 UntouchedFrameLimit = Settings.FrameLimit;

	ApexBootSettingsIo::Parse(Hand, Settings);

	TestEqual(TEXT("spaces around the x, width"), Settings.Resolution.X, 1280);
	TestEqual(TEXT("spaces around the x, height"), Settings.Resolution.Y, 720);
	TestEqual(TEXT("window mode is case-insensitive"),
		Settings.WindowMode, static_cast<int32>(EWindowMode::WindowedFullscreen));
	TestTrue(TEXT("'yes' is a bool"), Settings.bVSync);
	TestEqual(TEXT("an absent key keeps its default"), Settings.FrameLimit, UntouchedFrameLimit);
	TestEqual(TEXT("an absent section keeps its defaults"), Settings.ServerHost, UntouchedHost);

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexBootSettingsBadValuesTest,
	"ApexSim.Settings.BootSettingsBadValues",
	ApexTestFlags)

bool FApexBootSettingsBadValuesTest::RunTest(const FString& Parameters)
{
	// One bad line must not cost the player the rest of the file, and must not
	// leave a value that would stop the game being usable.
	AddExpectedError(TEXT("settings.yml line"), EAutomationExpectedErrorFlags::Contains, 0);

	const FString Broken =
		TEXT("display:\n")
		TEXT("  resolution: enormous\n")
		TEXT("  window_mode: cinema\n")
		TEXT("  frame_limit: -5\n")
		TEXT("  brightness: 11\n")
		TEXT("server:\n")
		TEXT("  port: 70000\n")
		TEXT("  host: 192.168.1.50\n");

	const FApexBootSettings Defaults;
	FApexBootSettings Settings;
	ApexBootSettingsIo::Parse(Broken, Settings);

	TestEqual(TEXT("bad resolution keeps the default width"), Settings.Resolution.X, Defaults.Resolution.X);
	TestEqual(TEXT("bad resolution keeps the default height"), Settings.Resolution.Y, Defaults.Resolution.Y);
	TestEqual(TEXT("unknown window mode keeps the default"), Settings.WindowMode, Defaults.WindowMode);
	TestEqual(TEXT("negative frame limit keeps the default"), Settings.FrameLimit, Defaults.FrameLimit);
	TestEqual(TEXT("out-of-range port keeps the default"), Settings.ServerPort, Defaults.ServerPort);
	TestEqual(TEXT("the good line after the bad ones still lands"),
		Settings.ServerHost, FString(TEXT("192.168.1.50")));

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexBootSettingsIpv6Test,
	"ApexSim.Settings.BootSettingsIpv6Host",
	ApexTestFlags)

bool FApexBootSettingsIpv6Test::RunTest(const FString& Parameters)
{
	// The key/value split takes the first colon only, so a host that is nothing
	// but colons still arrives whole.
	FApexBootSettings Settings;
	ApexBootSettingsIo::Parse(TEXT("server:\n  host: ::1\n"), Settings);

	TestEqual(TEXT("IPv6 loopback survives the split"), Settings.ServerHost, FString(TEXT("::1")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
