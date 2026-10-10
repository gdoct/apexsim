#include "ApexBootSettings.h"
#include "ApexTestCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

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
	Written.Screens = 3;
	Written.ServerHost = TEXT("race.example.net");
	Written.ServerPort = 9100;
	Written.bShowLauncher = false;
	Written.ServerTls.Mode = EApexTlsMode::On;
	Written.ServerTls.bVerify = false;
	Written.ServerTls.Fingerprint =
		TEXT("5E:2B:8C:01:9A:44:77:10:FE:3D:6A:C2:91:0B:58:E7:24:9F:D3:6C:A1:08:BB:4E:72:F5:19:3A:C6:0D:E8:57");

	FApexBootSettings Read;
	ApexBootSettingsIo::Parse(ApexBootSettingsIo::Serialise(Written), Read);

	// TestEqual has no FIntPoint overload, so the edges are compared separately.
	TestEqual(TEXT("resolution width survives"), Read.Resolution.X, Written.Resolution.X);
	TestEqual(TEXT("resolution height survives"), Read.Resolution.Y, Written.Resolution.Y);
	TestEqual(TEXT("window mode survives"), Read.WindowMode, Written.WindowMode);
	TestTrue(TEXT("vsync survives"), Read.bVSync);
	TestEqual(TEXT("uncapped frame limit survives"), Read.FrameLimit, 0);
	TestEqual(TEXT("a triple survives"), Read.Screens, 3);
	TestEqual(TEXT("host survives"), Read.ServerHost, Written.ServerHost);
	TestEqual(TEXT("port survives"), Read.ServerPort, Written.ServerPort);
	TestFalse(TEXT("the launcher's show flag survives the game's rewrite"), Read.bShowLauncher);
	TestTrue(TEXT("tls: on survives"), Read.ServerTls.Mode == EApexTlsMode::On);
	TestFalse(TEXT("tls_verify: false survives"), Read.ServerTls.bVerify);
	TestEqual(TEXT("the pinned fingerprint survives"), Read.ServerTls.Fingerprint, Written.ServerTls.Fingerprint);

	// And the defaults: auto, verified, no pin, written as an empty key.
	const FApexBootSettings Defaults;
	const FString DefaultText = ApexBootSettingsIo::Serialise(Defaults);
	TestTrue(TEXT("the default file says tls: auto"), DefaultText.Contains(TEXT("  tls: auto\n")));
	TestTrue(TEXT("and tls_verify: true"), DefaultText.Contains(TEXT("  tls_verify: true\n")));
	FApexBootSettings ReadDefaults;
	ReadDefaults.ServerTls.Fingerprint = TEXT("stale");
	ApexBootSettingsIo::Parse(DefaultText, ReadDefaults);
	TestTrue(TEXT("default mode reads back"), ReadDefaults.ServerTls.Mode == EApexTlsMode::Auto);
	TestTrue(TEXT("default verify reads back"), ReadDefaults.ServerTls.bVerify);
	TestTrue(TEXT("an empty tls_fingerprint clears the pin"), ReadDefaults.ServerTls.Fingerprint.IsEmpty());

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexBootSettingsTlsTest,
	"ApexSim.Settings.BootSettingsTls",
	ApexTestFlags)

bool FApexBootSettingsTlsTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("settings.yml line"), EAutomationExpectedErrorFlags::Contains, 0);

	// A file from before the keys: TLS is auto and verified.
	{
		FApexBootSettings Settings;
		ApexBootSettingsIo::Parse(TEXT("server:\n  host: 10.0.0.2\n  port: 9000\n"), Settings);
		TestTrue(TEXT("no tls key is auto"), Settings.ServerTls.Mode == EApexTlsMode::Auto);
		TestTrue(TEXT("no tls_verify key verifies"), Settings.ServerTls.bVerify);
		TestFalse(TEXT("no pin"), Settings.ServerTls.IsPinned());
	}

	// Hand-written: upper-case mode, a bool spelt "no", a fingerprint pasted as
	// lower-case hex with no colons (and a comment after it).
	{
		FApexBootSettings Settings;
		ApexBootSettingsIo::Parse(
			TEXT("server:\n")
			TEXT("  tls: OFF\n")
			TEXT("  tls_verify: no\n")
			TEXT("  tls_fingerprint: 5e2b8c019a447710fe3d6ac2910b58e7249fd36ca108bb4e72f5193ac60de857 # the LAN box\n"),
			Settings);
		TestTrue(TEXT("OFF is off"), Settings.ServerTls.Mode == EApexTlsMode::Off);
		TestFalse(TEXT("no is false"), Settings.ServerTls.bVerify);
		TestEqual(TEXT("the fingerprint is kept in the AB:CD spelling"), Settings.ServerTls.Fingerprint,
			FString(TEXT("5E:2B:8C:01:9A:44:77:10:FE:3D:6A:C2:91:0B:58:E7:24:9F:D3:6C:A1:08:BB:4E:72:F5:19:3A:C6:0D:E8:57")));
	}

	// Bad values keep what was there.
	{
		FApexBootSettings Settings;
		Settings.ServerTls.Fingerprint =
			TEXT("5E:2B:8C:01:9A:44:77:10:FE:3D:6A:C2:91:0B:58:E7:24:9F:D3:6C:A1:08:BB:4E:72:F5:19:3A:C6:0D:E8:57");
		ApexBootSettingsIo::Parse(
			TEXT("server:\n")
			TEXT("  tls: sometimes\n")
			TEXT("  tls_verify: perhaps\n")
			TEXT("  tls_fingerprint: 5e2b8c\n"),
			Settings);
		TestTrue(TEXT("a bad mode keeps auto"), Settings.ServerTls.Mode == EApexTlsMode::Auto);
		TestTrue(TEXT("a bad bool keeps verifying"), Settings.ServerTls.bVerify);
		TestTrue(TEXT("a short fingerprint keeps the old pin"), Settings.ServerTls.Fingerprint.StartsWith(TEXT("5E:2B:8C:01")));
	}
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
		TEXT("  screens: 2\n")
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
	TestEqual(TEXT("two screens is not a layout; keeps the default"), Settings.Screens, Defaults.Screens);
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
