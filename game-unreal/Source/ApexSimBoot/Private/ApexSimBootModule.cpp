#include "ApexStartupSplash.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogApexSimBoot, Log, All);

namespace
{
	/**
	 * Longest a claimed hold may last before the window is shown regardless.
	 * Above the game's own limit (apexsim.splash.MaxSeconds): this is for a game
	 * that has stopped asking, not for a slow demo.
	 */
	constexpr double WatchdogSeconds = 45.0;
}

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <dwmapi.h>

namespace ApexSplashWindows
{
	// Named here rather than taken from the SDK: DWMWA_WINDOW_CORNER_PREFERENCE
	// only exists in Windows 11 SDKs.
	constexpr DWORD DwmCloak = 13;              // DWMWA_CLOAK
	constexpr DWORD DwmCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
	constexpr DWORD DwmCornerRound = 2;         // DWMWCP_ROUND

	const TCHAR* const ClassName = TEXT("ApexSimSplashClass");

	HWND Splash = nullptr;
	HBITMAP Bitmap = nullptr;
	int32 BitmapWidth = 0;
	int32 BitmapHeight = 0;
	bool bClassRegistered = false;

	/** Set from inside a hook when the hold could not be set up; the module lets go. */
	bool bHoldAbandoned = false;

	HHOOK CreateHook = nullptr;
	HHOOK ShowHook = nullptr;
	/** The game window, once seen being created; cloaked until the reveal. */
	HWND GameWindow = nullptr;
	bool bGameWindowCloaked = false;

	bool SetCloaked(HWND Window, bool bCloak)
	{
		if (!Window || !IsWindow(Window))
		{
			return false;
		}
		BOOL Value = bCloak ? TRUE : FALSE;
		const HRESULT Result = DwmSetWindowAttribute(Window, static_cast<DWMWINDOWATTRIBUTE>(DwmCloak), &Value, sizeof(Value));
		if (FAILED(Result))
		{
			UE_LOG(LogApexSimBoot, Warning, TEXT("Startup splash: could not %s the game window (0x%08x)"),
				bCloak ? TEXT("cloak") : TEXT("uncloak"), static_cast<uint32>(Result));
			return false;
		}
		return true;
	}

	void CloakGameWindow(HWND Window)
	{
		if (GameWindow && GameWindow != Window && bGameWindowCloaked)
		{
			SetCloaked(GameWindow, false);
		}
		GameWindow = Window;
		bGameWindowCloaked = SetCloaked(Window, true);
	}

	void UncloakGameWindow()
	{
		if (GameWindow && bGameWindowCloaked)
		{
			SetCloaked(GameWindow, false);
		}
		bGameWindowCloaked = false;
	}

	bool IsUnrealWindowClass(HWND Window)
	{
		wchar_t Name[64];
		return GetClassNameW(Window, Name, UE_ARRAY_COUNT(Name)) > 0
			&& FCString::Strcmp(Name, TEXT("UnrealWindow")) == 0;
	}

	HWND FindExistingGameWindow()
	{
		HWND Found = nullptr;
		EnumWindows([](HWND Window, LPARAM Out) -> BOOL
		{
			DWORD Process = 0;
			GetWindowThreadProcessId(Window, &Process);
			if (Process == GetCurrentProcessId() && IsUnrealWindowClass(Window) && !GetParent(Window))
			{
				*reinterpret_cast<HWND*>(Out) = Window;
				return 0;
			}
			return 1;
		}, reinterpret_cast<LPARAM>(&Found));
		return Found;
	}

	void RemoveHooks()
	{
		if (CreateHook)
		{
			UnhookWindowsHookEx(CreateHook);
			CreateHook = nullptr;
		}
		if (ShowHook)
		{
			UnhookWindowsHookEx(ShowHook);
			ShowHook = nullptr;
		}
	}

	bool OpenSplash();
	void CloseAll();

	/**
	 * The first sight of the game window: put the copy of the splash up, and
	 * hide the window now if it is already on screen. The copy waits until now
	 * because the engine's splash, and the DPI awareness its placement depends
	 * on, only exist after this module loads.
	 */
	void TakeGameWindow(HWND Window, bool bHideNow)
	{
		if (bHideNow)
		{
			CloakGameWindow(Window);
			UE_LOG(LogApexSimBoot, Log, TEXT("Startup splash: game window %s"),
				bGameWindowCloaked ? TEXT("hidden") : TEXT("could not be hidden"));
		}
		else
		{
			GameWindow = Window;
		}
		if (!Splash && !OpenSplash())
		{
			UE_LOG(LogApexSimBoot, Log, TEXT("Startup splash: no splash bitmap, or its window could not be made; not holding"));
			CloseAll();
			bHoldAbandoned = true;
		}
	}

	/**
	 * Notes the game window as it is made. DWM refuses to cloak a window this
	 * early (E_HANDLE), so the cloak itself waits for ShowHookProc.
	 */
	LRESULT CALLBACK CreateHookProc(int Code, WPARAM WParam, LPARAM LParam)
	{
		if (Code == HCBT_CREATEWND && !GameWindow && !bHoldAbandoned)
		{
			const HWND Window = reinterpret_cast<HWND>(WParam);
			const CBT_CREATEWND* Create = reinterpret_cast<CBT_CREATEWND*>(LParam);
			if (Create && Create->lpcs && !Create->lpcs->hwndParent && IsUnrealWindowClass(Window))
			{
				TakeGameWindow(Window, /*bHideNow*/ false);
			}
		}
		return CallNextHookEx(CreateHook, Code, WParam, LParam);
	}

	/** Cloaks the game window as it is shown: WM_SHOWWINDOW arrives before it is on screen. */
	LRESULT CALLBACK ShowHookProc(int Code, WPARAM WParam, LPARAM LParam)
	{
		if (Code == HC_ACTION && GameWindow && !bGameWindowCloaked)
		{
			const CWPSTRUCT* Message = reinterpret_cast<CWPSTRUCT*>(LParam);
			if (Message && Message->hwnd == GameWindow && Message->message == WM_SHOWWINDOW && Message->wParam)
			{
				bGameWindowCloaked = SetCloaked(GameWindow, true);
				UE_LOG(LogApexSimBoot, Log, TEXT("Startup splash: game window %s"),
					bGameWindowCloaked ? TEXT("hidden") : TEXT("could not be hidden"));
			}
		}
		return CallNextHookEx(ShowHook, Code, WParam, LParam);
	}

	LRESULT CALLBACK SplashWndProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
	{
		switch (Message)
		{
		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT Paint;
			const HDC Target = BeginPaint(Window, &Paint);
			if (Bitmap)
			{
				RECT Client;
				GetClientRect(Window, &Client);
				const HDC Source = CreateCompatibleDC(Target);
				const HGDIOBJ Previous = SelectObject(Source, Bitmap);
				if (Client.right == BitmapWidth && Client.bottom == BitmapHeight)
				{
					BitBlt(Target, 0, 0, BitmapWidth, BitmapHeight, Source, 0, 0, SRCCOPY);
				}
				else
				{
					SetStretchBltMode(Target, HALFTONE);
					StretchBlt(Target, 0, 0, Client.right, Client.bottom, Source, 0, 0, BitmapWidth, BitmapHeight, SRCCOPY);
				}
				SelectObject(Source, Previous);
				DeleteDC(Source);
			}
			EndPaint(Window, &Paint);
			return 0;
		}

		// The player cannot dismiss it; the game does.
		case WM_CLOSE:
			return 0;

		default:
			return DefWindowProcW(Window, Message, WParam, LParam);
		}
	}

	FString FindSplashBitmap()
	{
		// The engine's own lookup order (FGenericPlatformSplash::GetSplashPath).
		for (const FString& Dir : { FPaths::ProjectContentDir(), FPaths::EngineContentDir() })
		{
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, TEXT("Splash/Splash.bmp")));
			if (FPaths::FileExists(Path))
			{
				return Path;
			}
		}
		return FString();
	}

	HWND FindEngineSplash()
	{
		for (HWND Candidate = FindWindowExW(nullptr, nullptr, TEXT("SplashScreenClass"), nullptr); Candidate;
			 Candidate = FindWindowExW(nullptr, Candidate, TEXT("SplashScreenClass"), nullptr))
		{
			DWORD Process = 0;
			GetWindowThreadProcessId(Candidate, &Process);
			if (Process == GetCurrentProcessId())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	bool OpenSplash()
	{
		const FString Path = FindSplashBitmap();
		if (Path.IsEmpty())
		{
			return false;
		}
		Bitmap = static_cast<HBITMAP>(LoadImageW(nullptr, *Path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
		if (!Bitmap)
		{
			return false;
		}
		BITMAP Info;
		GetObjectW(Bitmap, sizeof(Info), &Info);
		BitmapWidth = Info.bmWidth;
		BitmapHeight = Info.bmHeight;

		const HINSTANCE Instance = GetModuleHandleW(nullptr);
		WNDCLASSW Class = {};
		Class.lpfnWndProc = &SplashWndProc;
		Class.hInstance = Instance;
		Class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		Class.lpszClassName = ClassName;
		bClassRegistered = RegisterClassW(&Class) != 0;
		if (!bClassRegistered)
		{
			return false;
		}

		// Exactly over the engine's splash, which is centred on the primary
		// display at the bitmap's size; its own rectangle is the authority, in
		// case DPI scaling put it somewhere else.
		const HWND EngineSplash = FindEngineSplash();
		RECT Rect;
		if (!EngineSplash || !GetWindowRect(EngineSplash, &Rect))
		{
			Rect.left = (GetSystemMetrics(SM_CXSCREEN) - BitmapWidth) / 2;
			Rect.top = (GetSystemMetrics(SM_CYSCREEN) - BitmapHeight) / 2;
			Rect.right = Rect.left + BitmapWidth;
			Rect.bottom = Rect.top + BitmapHeight;
		}

		// A tool window, like the engine's: no taskbar button of its own.
		Splash = CreateWindowExW(WS_EX_TOOLWINDOW, ClassName, TEXT("ApexSim"), WS_POPUP,
			Rect.left, Rect.top, Rect.right - Rect.left, Rect.bottom - Rect.top,
			nullptr, nullptr, Instance, nullptr);
		if (!Splash)
		{
			return false;
		}
		DWORD Corners = DwmCornerRound;
		DwmSetWindowAttribute(Splash, static_cast<DWMWINDOWATTRIBUTE>(DwmCornerPreference), &Corners, sizeof(Corners));

		// Just beneath the engine's splash: when that one closes, this is what
		// was underneath, pixel for pixel.
		SetWindowPos(Splash, EngineSplash ? EngineSplash : HWND_TOP, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
		UpdateWindow(Splash);
		return true;
	}

	void BeginFade()
	{
		if (!Splash)
		{
			return;
		}
		// Above the game window it now covers, then see-through from here on.
		SetWindowPos(Splash, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		SetWindowLongPtrW(Splash, GWL_EXSTYLE, GetWindowLongPtrW(Splash, GWL_EXSTYLE) | WS_EX_LAYERED);
		SetLayeredWindowAttributes(Splash, 0, 255, LWA_ALPHA);
	}

	/**
	 * The game window was activated while hidden; keep keyboard focus on it
	 * rather than on the splash, unless the player has moved to another app.
	 */
	void FocusGameWindow()
	{
		const HWND Foreground = GetForegroundWindow();
		DWORD Process = 0;
		GetWindowThreadProcessId(Foreground, &Process);
		if (GameWindow && Foreground != GameWindow && Process == GetCurrentProcessId())
		{
			SetForegroundWindow(GameWindow);
		}
	}

	void SetSplashOpacity(float Opacity)
	{
		if (Splash)
		{
			SetLayeredWindowAttributes(Splash, 0, static_cast<BYTE>(FMath::Clamp(FMath::RoundToInt(Opacity * 255.0f), 0, 255)), LWA_ALPHA);
		}
	}

	void CloseAll()
	{
		RemoveHooks();
		UncloakGameWindow();
		GameWindow = nullptr;
		if (Splash)
		{
			DestroyWindow(Splash);
			Splash = nullptr;
		}
		if (bClassRegistered)
		{
			UnregisterClassW(ClassName, GetModuleHandleW(nullptr));
			bClassRegistered = false;
		}
		if (Bitmap)
		{
			DeleteObject(Bitmap);
			Bitmap = nullptr;
		}
	}
}

#include "Windows/HideWindowsPlatformTypes.h"

#endif // PLATFORM_WINDOWS

namespace ApexStartupSplash
{
	namespace
	{
		bool bHolding = false;
		bool bClaimed = false;
		bool bFading = false;
		double FadeStart = 0.0;
		float FadeDuration = 0.0f;
		/** When engine init finished; the watchdog counts from here. */
		double InitCompleteTime = 0.0;
		FTSTicker::FDelegateHandle TickerHandle;

		void Close()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
#if PLATFORM_WINDOWS
			ApexSplashWindows::CloseAll();
#endif
			bHolding = false;
			bFading = false;
		}

		/**
		 * End of engine init: the menu map is loaded and the game window exists.
		 * A hold nobody has claimed by now never will be.
		 */
		void HandleEngineInitComplete()
		{
			InitCompleteTime = FPlatformTime::Seconds();
#if PLATFORM_WINDOWS
			ApexSplashWindows::RemoveHooks();
#endif
			if (bHolding && (!bClaimed
#if PLATFORM_WINDOWS
				|| ApexSplashWindows::bHoldAbandoned || !ApexSplashWindows::Splash
#endif
				))
			{
				UE_LOG(LogApexSimBoot, Log, TEXT("Startup splash: nothing claimed the hold, or it never got a window; showing the game"));
				Close();
			}
		}

		bool Tick(float)
		{
			const double Now = FPlatformTime::Seconds();
#if PLATFORM_WINDOWS
			if (ApexSplashWindows::bHoldAbandoned)
			{
				TickerHandle.Reset();
				Close();
				return false;
			}
#endif
			if (bFading)
			{
				const float Alpha = FadeDuration > 0.0f
					? FMath::Clamp(static_cast<float>((Now - FadeStart) / FadeDuration), 0.0f, 1.0f)
					: 1.0f;
#if PLATFORM_WINDOWS
				ApexSplashWindows::SetSplashOpacity(1.0f - FMath::SmoothStep(0.0f, 1.0f, Alpha));
#endif
				if (Alpha >= 1.0f)
				{
					TickerHandle.Reset();
					Close();
					return false;
				}
			}
			else if (InitCompleteTime > 0.0 && Now - InitCompleteTime > WatchdogSeconds)
			{
				UE_LOG(LogApexSimBoot, Warning, TEXT("Startup splash: still held after %.0f s; showing the game"), WatchdogSeconds);
				TickerHandle.Reset();
				Close();
				return false;
			}
			return true;
		}
	}

	bool IsHolding()
	{
		return bHolding;
	}

	void Claim()
	{
		bClaimed = bHolding;
	}

	bool ConfirmGameWindow(void* OsWindowHandle)
	{
#if PLATFORM_WINDOWS
		if (!bHolding)
		{
			return false;
		}
		const HWND Handle = static_cast<HWND>(OsWindowHandle);
		if (Handle && (Handle != ApexSplashWindows::GameWindow || !ApexSplashWindows::bGameWindowCloaked))
		{
			ApexSplashWindows::CloakGameWindow(Handle);
		}
		return ApexSplashWindows::bGameWindowCloaked;
#else
		return false;
#endif
	}

	void Reveal(float FadeSeconds)
	{
		if (!bHolding || bFading)
		{
			return;
		}
#if PLATFORM_WINDOWS
		ApexSplashWindows::BeginFade();
		ApexSplashWindows::UncloakGameWindow();
		ApexSplashWindows::FocusGameWindow();
#endif
		bFading = true;
		FadeStart = FPlatformTime::Seconds();
		FadeDuration = FadeSeconds;
	}

	void Release()
	{
		Close();
	}

	void Start()
	{
#if PLATFORM_WINDOWS
		if (GIsEditor || IsRunningCommandlet() || IsRunningDedicatedServer() || !FApp::CanEverRender())
		{
			return;
		}
		const TCHAR* const CommandLine = FCommandLine::Get();
		if (FParse::Param(CommandLine, TEXT("nosplash")) || FParse::Param(CommandLine, TEXT("ApexNoSplashHold"))
			// Nothing to wait for: these runs never play the menu's demo race.
			|| FParse::Param(CommandLine, TEXT("ApexNoDemo")) || FParse::Param(CommandLine, TEXT("ApexAutoRace"))
			|| FCString::Strifind(CommandLine, TEXT("-ApexReplay=")) != nullptr)
		{
			return;
		}
		const DWORD Thread = GetCurrentThreadId();
		ApexSplashWindows::CreateHook = SetWindowsHookExW(WH_CBT, &ApexSplashWindows::CreateHookProc, nullptr, Thread);
		ApexSplashWindows::ShowHook = SetWindowsHookExW(WH_CALLWNDPROC, &ApexSplashWindows::ShowHookProc, nullptr, Thread);
		bHolding = true;
		// In case the engine has already made its window (a loading phase change).
		if (const HWND Existing = ApexSplashWindows::FindExistingGameWindow())
		{
			ApexSplashWindows::TakeGameWindow(Existing, /*bHideNow*/ IsWindowVisible(Existing) != 0);
		}
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
		FCoreDelegates::OnFEngineLoopInitComplete.AddStatic(&HandleEngineInitComplete);
		UE_LOG(LogApexSimBoot, Log, TEXT("Startup splash: holding"));
#endif
	}
}

class FApexSimBootModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		ApexStartupSplash::Start();
	}

	virtual void ShutdownModule() override
	{
		ApexStartupSplash::Release();
	}
};

IMPLEMENT_MODULE(FApexSimBootModule, ApexSimBoot)
