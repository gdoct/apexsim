#include "ApexBootSettings.h"

#include "ApexSim.h"
#include "ApexNetSubsystem.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/PlatformProperties.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** Smallest resolution worth honouring; below this the menus stop laying out. */
	constexpr int32 MinResolutionEdge = 320;

	bool ParseBool(const FString& Value, bool& Out)
	{
		const FString Lower = Value.ToLower();
		if (Lower == TEXT("true") || Lower == TEXT("yes") || Lower == TEXT("on") || Lower == TEXT("1"))
		{
			Out = true;
			return true;
		}
		if (Lower == TEXT("false") || Lower == TEXT("no") || Lower == TEXT("off") || Lower == TEXT("0"))
		{
			Out = false;
			return true;
		}
		return false;
	}

	bool ParseInt(const FString& Value, int32 Min, int32 Max, int32& Out)
	{
		if (!Value.IsNumeric())
		{
			return false;
		}

		const int64 Parsed = FCString::Atoi64(*Value);
		if (Parsed < Min || Parsed > Max)
		{
			return false;
		}

		Out = static_cast<int32>(Parsed);
		return true;
	}

	/** "1920x1080", with or without spaces around the x. */
	bool ParseResolution(const FString& Value, FIntPoint& Out)
	{
		FString Left;
		FString Right;
		if (!Value.Split(TEXT("x"), &Left, &Right, ESearchCase::IgnoreCase))
		{
			return false;
		}

		int32 Width = 0;
		int32 Height = 0;
		if (!ParseInt(Left.TrimStartAndEnd(), MinResolutionEdge, 32768, Width)
			|| !ParseInt(Right.TrimStartAndEnd(), MinResolutionEdge, 32768, Height))
		{
			return false;
		}

		Out = FIntPoint(Width, Height);
		return true;
	}

	bool ParseWindowMode(const FString& Value, int32& Out)
	{
		const FString Lower = Value.ToLower();
		if (Lower == TEXT("fullscreen"))
		{
			Out = static_cast<int32>(EWindowMode::Fullscreen);
			return true;
		}
		// "borderless" is what players call it; Unreal calls the same thing a
		// windowed fullscreen. Both spellings are accepted so neither surprises.
		if (Lower == TEXT("borderless") || Lower == TEXT("windowed_fullscreen") || Lower == TEXT("borderless_windowed"))
		{
			Out = static_cast<int32>(EWindowMode::WindowedFullscreen);
			return true;
		}
		if (Lower == TEXT("windowed") || Lower == TEXT("window"))
		{
			Out = static_cast<int32>(EWindowMode::Windowed);
			return true;
		}
		return false;
	}

	const TCHAR* WindowModeName(int32 Mode)
	{
		switch (static_cast<EWindowMode::Type>(Mode))
		{
		case EWindowMode::WindowedFullscreen: return TEXT("borderless");
		case EWindowMode::Windowed:           return TEXT("windowed");
		default:                              return TEXT("fullscreen");
		}
	}

	void WarnBadValue(int32 LineNumber, const TCHAR* Key, const FString& Value, const TCHAR* Expected)
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("settings.yml line %d: '%s' is not a valid %s (%s) - keeping the current value"),
			LineNumber, *Value, Key, Expected);
	}
}

// --- File format -------------------------------------------------------------

void ApexBootSettingsIo::Parse(const FString& Text, FApexBootSettings& InOut)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

	FString Section;

	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		const int32 LineNumber = Index + 1;
		FString Line = Lines[Index];

		// Nothing in this file is ever quoted, so a '#' anywhere starts a comment.
		int32 Hash = INDEX_NONE;
		if (Line.FindChar(TEXT('#'), Hash))
		{
			Line.LeftInline(Hash);
		}

		// Indentation is the only thing separating a key inside a section from a
		// section header, so it has to be read before the line is trimmed.
		const bool bIndented = Line.StartsWith(TEXT(" ")) || Line.StartsWith(TEXT("\t"));
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty())
		{
			continue;
		}

		FString Key;
		FString Value;
		if (!Line.Split(TEXT(":"), &Key, &Value))
		{
			UE_LOG(LogApexSim, Warning,
				TEXT("settings.yml line %d: '%s' is not 'key: value' - ignored"), LineNumber, *Line);
			continue;
		}

		Key = Key.TrimStartAndEnd().ToLower();
		// Split takes the first colon, so an IPv6 host keeps the rest of its own.
		Value = Value.TrimStartAndEnd().TrimQuotes();

		if (Value.IsEmpty() && !bIndented)
		{
			Section = Key;
			continue;
		}

		const FString Path = Section.IsEmpty() ? Key : Section + TEXT(".") + Key;

		if (Path == TEXT("display.resolution"))
		{
			if (!ParseResolution(Value, InOut.Resolution))
			{
				WarnBadValue(LineNumber, TEXT("resolution"), Value, TEXT("expected WIDTHxHEIGHT, e.g. 1920x1080"));
			}
		}
		else if (Path == TEXT("display.window_mode"))
		{
			if (!ParseWindowMode(Value, InOut.WindowMode))
			{
				WarnBadValue(LineNumber, TEXT("window_mode"), Value, TEXT("expected fullscreen, borderless or windowed"));
			}
		}
		else if (Path == TEXT("display.vsync"))
		{
			if (!ParseBool(Value, InOut.bVSync))
			{
				WarnBadValue(LineNumber, TEXT("vsync"), Value, TEXT("expected true or false"));
			}
		}
		else if (Path == TEXT("display.frame_limit"))
		{
			if (!ParseInt(Value, 0, 1000, InOut.FrameLimit))
			{
				WarnBadValue(LineNumber, TEXT("frame_limit"), Value, TEXT("expected 0 (uncapped) to 1000"));
			}
		}
		else if (Path == TEXT("display.screens"))
		{
			int32 Screens = 0;
			if (ParseInt(Value, 1, 3, Screens) && Screens != 2)
			{
				InOut.Screens = Screens;
			}
			else
			{
				WarnBadValue(LineNumber, TEXT("screens"), Value, TEXT("expected 1 (one monitor) or 3 (a triple)"));
			}
		}
		else if (Path == TEXT("server.host"))
		{
			if (Value.IsEmpty())
			{
				WarnBadValue(LineNumber, TEXT("host"), Value, TEXT("expected a host name or address"));
			}
			else
			{
				InOut.ServerHost = Value;
			}
		}
		else if (Path == TEXT("server.port"))
		{
			if (!ParseInt(Value, 1, 65535, InOut.ServerPort))
			{
				WarnBadValue(LineNumber, TEXT("port"), Value, TEXT("expected 1 to 65535"));
			}
		}
		else if (Path == TEXT("server.tls"))
		{
			if (!ApexTls::ParseMode(Value, InOut.ServerTls.Mode))
			{
				WarnBadValue(LineNumber, TEXT("tls"), Value, TEXT("expected auto, on or off"));
			}
		}
		else if (Path == TEXT("server.tls_verify"))
		{
			if (!ParseBool(Value, InOut.ServerTls.bVerify))
			{
				WarnBadValue(LineNumber, TEXT("tls_verify"), Value, TEXT("expected true or false"));
			}
		}
		else if (Path == TEXT("server.tls_fingerprint"))
		{
			if (Value.IsEmpty())
			{
				InOut.ServerTls.Fingerprint.Reset();
			}
			else if (!ApexTls::CanonicalFingerprint(Value, InOut.ServerTls.Fingerprint))
			{
				WarnBadValue(LineNumber, TEXT("tls_fingerprint"), Value,
					TEXT("expected a SHA-256 fingerprint: 64 hex digits, colons optional"));
			}
		}
		else if (Path == TEXT("launcher.show"))
		{
			if (!ParseBool(Value, InOut.bShowLauncher))
			{
				WarnBadValue(LineNumber, TEXT("show"), Value, TEXT("expected true or false"));
			}
		}
		else
		{
			UE_LOG(LogApexSim, Warning,
				TEXT("settings.yml line %d: unknown setting '%s' - ignored"), LineNumber, *Path);
		}
	}
}

FString ApexBootSettingsIo::Serialise(const FApexBootSettings& Settings)
{
	// scripts/build_release.ps1 ships a settings.sample.yml with this same shape
	// and the shipped defaults, so a player can read the file before the first
	// run creates one. Keep the two in step.

	return FString::Printf(
		TEXT("# ApexSim settings.\n")
		TEXT("#\n")
		TEXT("# Edit this file to change how the game starts up. It is rewritten whenever\n")
		TEXT("# these settings are changed from inside the game, so comments of your own\n")
		TEXT("# will not survive that. Delete the file to go back to the defaults.\n")
		TEXT("\n")
		TEXT("display:\n")
		TEXT("  # WIDTHxHEIGHT, e.g. 2560x1440. Ignored in borderless, which always\n")
		TEXT("  # takes the size of the monitor it opens on.\n")
		TEXT("  resolution: %dx%d\n")
		TEXT("  # fullscreen | borderless | windowed\n")
		TEXT("  window_mode: %s\n")
		TEXT("  vsync: %s\n")
		TEXT("  # Frames per second, or 0 for uncapped.\n")
		TEXT("  frame_limit: %d\n")
		TEXT("  # 1 for one monitor, or 3 for a triple-monitor rig: the window then spans\n")
		TEXT("  # the three monitors standing in a row and each shows its own view. The\n")
		TEXT("  # rig's measurements are under Settings > Graphics > Screens.\n")
		TEXT("  screens: %d\n")
		TEXT("\n")
		TEXT("server:\n")
		TEXT("  # The server the game connects to when it starts. 127.0.0.1 is a server\n")
		TEXT("  # on this machine, such as the one the launcher starts for you.\n")
		TEXT("  host: %s\n")
		TEXT("  port: %d\n")
		TEXT("  # TLS on the connection: auto | on | off. auto tries TLS and falls back\n")
		TEXT("  # to plaintext only when the server does not speak it; on never falls\n")
		TEXT("  # back. Telemetry and input (UDP) are never encrypted.\n")
		TEXT("  tls: %s\n")
		TEXT("  # Check the server's certificate against the system's trusted roots and\n")
		TEXT("  # the host name. false encrypts without checking: development only.\n")
		TEXT("  tls_verify: %s\n")
		TEXT("  # Trust exactly this certificate instead (SHA-256, as the server's\n")
		TEXT("  # operator gives it, or as the game logs it when it refuses one): how a\n")
		TEXT("  # self-signed certificate is trusted. Empty: none. Cleared when host changes.\n")
		TEXT("  tls_fingerprint: %s\n")
		TEXT("\n")
		TEXT("launcher:\n")
		TEXT("  # false: launcher.exe starts the game straight away, without a server, and\n")
		TEXT("  # shows no window. Run launcher.exe --show to bring the window back.\n")
		TEXT("  show: %s\n"),
		Settings.Resolution.X, Settings.Resolution.Y,
		WindowModeName(Settings.WindowMode),
		Settings.bVSync ? TEXT("true") : TEXT("false"),
		Settings.FrameLimit,
		Settings.Screens,
		*Settings.ServerHost,
		Settings.ServerPort,
		ApexTls::ModeName(Settings.ServerTls.Mode),
		Settings.ServerTls.bVerify ? TEXT("true") : TEXT("false"),
		*Settings.ServerTls.Fingerprint,
		Settings.bShowLauncher ? TEXT("true") : TEXT("false"));
}

// --- Subsystem ---------------------------------------------------------------

FString UApexBootSettingsSubsystem::FilePath()
{
	// In a packaged build ProjectDir is <Release>/Game/ApexSim/, so its parent is
	// the folder holding ApexSim.exe - which is where a player will look for a
	// file like this. In the editor there is no such folder, and ProjectDir
	// (game-unreal/) is the closest equivalent.
	const FString Dir = FPlatformProperties::RequiresCookedData()
		? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."))
		: FPaths::ProjectDir();

	return FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, TEXT("settings.yml")));
}

void UApexBootSettingsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const FString Path = FilePath();

	FString Text;
	bFileExisted = FFileHelper::LoadFileToString(Text, *Path);

	// The net subsystem takes the TLS options from here, so it must exist first.
	Collection.InitializeDependency<UApexNetSubsystem>();

	if (bFileExisted)
	{
		ApexBootSettingsIo::Parse(Text, Settings);
		UE_LOG(LogApexSim, Log, TEXT("Settings read from %s: %dx%d %s, server %s:%d (tls %s%s%s)"),
			*Path, Settings.Resolution.X, Settings.Resolution.Y,
			WindowModeName(Settings.WindowMode), *Settings.ServerHost, Settings.ServerPort,
			ApexTls::ModeName(Settings.ServerTls.Mode),
			Settings.ServerTls.IsPinned() ? TEXT(", pinned") : TEXT(""),
			!Settings.ServerTls.IsPinned() && !Settings.ServerTls.bVerify ? TEXT(", NOT verified") : TEXT(""));
		PushTlsOptions();
		return;
	}
	PushTlsOptions();

	// First run. 1920x1080 is a guess that is wrong on most machines, so the file
	// is created describing the display it is actually created on. The subsystems
	// that own these values may seed it further - see DidFileExist - but writing
	// now means a run that crashes before then still leaves a file to edit.
	if (const UGameUserSettings* User = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		Settings.Resolution = User->GetScreenResolution();
		Settings.WindowMode = static_cast<int32>(User->GetFullscreenMode());
	}

	UE_LOG(LogApexSim, Log, TEXT("No settings file at %s - writing one with the defaults"), *Path);
	Write();
}

void UApexBootSettingsSubsystem::SetDisplay(FIntPoint Resolution, int32 WindowMode, bool bVSync, int32 FrameLimit, int32 Screens)
{
	Screens = Screens == 3 ? 3 : 1;
	if (Settings.Resolution == Resolution
		&& Settings.WindowMode == WindowMode
		&& Settings.bVSync == bVSync
		&& Settings.FrameLimit == FrameLimit
		&& Settings.Screens == Screens)
	{
		return;
	}

	Settings.Resolution = Resolution;
	Settings.WindowMode = WindowMode;
	Settings.bVSync = bVSync;
	Settings.FrameLimit = FrameLimit;
	Settings.Screens = Screens;
	Write();
}

void UApexBootSettingsSubsystem::SetServer(const FString& Host, int32 Port)
{
	if (Host.IsEmpty() || (Settings.ServerHost == Host && Settings.ServerPort == Port))
	{
		return;
	}

	if (Settings.ServerTls.IsPinned() && !Settings.ServerHost.Equals(Host, ESearchCase::IgnoreCase))
	{
		// The pin is one server's certificate; kept, it would refuse the new one.
		UE_LOG(LogApexSim, Log, TEXT("Server host changed from %s to %s: dropping the pinned TLS fingerprint"),
			*Settings.ServerHost, *Host);
		Settings.ServerTls.Fingerprint.Reset();
		PushTlsOptions();
	}
	Settings.ServerHost = Host;
	Settings.ServerPort = Port;
	Write();
}

void UApexBootSettingsSubsystem::PushTlsOptions() const
{
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (UApexNetSubsystem* Net = GameInstance->GetSubsystem<UApexNetSubsystem>())
		{
			Net->SetTlsOptions(Settings.ServerTls);
		}
	}
}

void UApexBootSettingsSubsystem::Write() const
{
	const FString Path = FilePath();

	// UTF-8 without a BOM: the file is meant to be opened in whatever editor the
	// player has to hand, and a BOM trips up more of them than it helps.
	if (!FFileHelper::SaveStringToFile(
			ApexBootSettingsIo::Serialise(Settings), *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("Could not write %s - settings changed this run will not survive it"), *Path);
	}
}
