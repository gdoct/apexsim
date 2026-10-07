// Console commands that drive the menus by their text, for unattended checks:
//
//   apexsim.ui.Texts [filter]          log every visible text with its centre (viewport px)
//   apexsim.ui.Click <text> [#N]       click the Nth (0-based, top to bottom) visible text
//                                      matching <text> exactly (case-insensitive)
//   apexsim.ui.Mouse down|move|up X Y  press / drag / release the left button at a
//                                      viewport point
//
// Every click goes through Slate's own input pipeline (cursor move, button
// down, button up), so it lands on whatever a hand on the mouse would hit:
// a button under its label, a grid slot, a slider. Run them from
// -ApexExecAfter (the world timer) rather than from a widget's tick, where
// the hit-test grid is half built.

#include "ApexSim.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SViewport.h"

namespace ApexUiScript
{
	struct FFound
	{
		FString Text;
		FVector2D Centre;
	};

	/** Whether the widget and every container above it is shown (and on a switcher's active page). */
	bool IsShown(const UWidget* Widget)
	{
		const UWidget* Current = Widget;
		while (Current)
		{
			if (!Current->IsVisible())
			{
				return false;
			}
			const UPanelWidget* Parent = Current->GetParent();
			if (const UWidgetSwitcher* Switcher = Cast<UWidgetSwitcher>(Parent))
			{
				if (Switcher->GetActiveWidget() != Current)
				{
					return false;
				}
			}
			if (Parent)
			{
				Current = Parent;
				continue;
			}
			// The root of a user widget's tree: carry on from the user widget.
			const UWidgetTree* Tree = Cast<UWidgetTree>(Current->GetOuter());
			Current = Tree ? Cast<UUserWidget>(Tree->GetOuter()) : nullptr;
		}
		return true;
	}

	/** The game viewport's top-left in Slate's absolute space, and its DPI-free pixel scale. */
	bool ViewportOrigin(FVector2D& OutOrigin)
	{
		if (!GEngine || !GEngine->GameViewport)
		{
			return false;
		}
		const TSharedPtr<SViewport> Viewport = GEngine->GameViewport->GetGameViewportWidget();
		if (!Viewport.IsValid())
		{
			return false;
		}
		OutOrigin = FVector2D(Viewport->GetCachedGeometry().GetAbsolutePosition());
		return true;
	}

	/** Every visible text in the game world, top to bottom then left to right, in absolute space. */
	TArray<FFound> VisibleTexts(UWorld* World)
	{
		TArray<FFound> Found;
		for (TObjectIterator<UTextBlock> It; It; ++It)
		{
			UTextBlock* Text = *It;
			if (!Text || Text->GetWorld() != World || !Text->GetCachedWidget().IsValid() || !IsShown(Text))
			{
				continue;
			}
			const FGeometry& Geometry = Text->GetCachedGeometry();
			const FVector2D Size = FVector2D(Geometry.GetAbsoluteSize());
			if (Size.X < 1.0 || Size.Y < 1.0)
			{
				continue;
			}
			const FString Value = Text->GetText().ToString().TrimStartAndEnd();
			if (Value.IsEmpty())
			{
				continue;
			}
			Found.Add({ Value, FVector2D(Geometry.GetAbsolutePosition()) + Size * 0.5 });
		}
		Found.Sort([](const FFound& A, const FFound& B)
		{
			// Rows within 4 px count as one line.
			return FMath::Abs(A.Centre.Y - B.Centre.Y) > 4.0 ? A.Centre.Y < B.Centre.Y : A.Centre.X < B.Centre.X;
		});
		return Found;
	}

	void Send(const TCHAR* Verb, const FVector2D& Point)
	{
		static FVector2D Last = FVector2D::ZeroVector;
		FSlateApplication& Slate = FSlateApplication::Get();
		const bool bDown = FCString::Stricmp(Verb, TEXT("down")) == 0;
		const bool bUp = FCString::Stricmp(Verb, TEXT("up")) == 0;
		static bool bHeld = false;
		Slate.SetCursorPos(Point);
		const TSet<FKey> Held = bHeld ? TSet<FKey>({ EKeys::LeftMouseButton }) : TSet<FKey>();
		Slate.ProcessMouseMoveEvent(FPointerEvent(0, Point, Last, Held, FKey(), 0.0f, FModifierKeysState()));
		if (bDown)
		{
			bHeld = true;
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(0, Point, Point,
				TSet<FKey>({ EKeys::LeftMouseButton }), EKeys::LeftMouseButton, 0.0f, FModifierKeysState()));
		}
		else if (bUp)
		{
			bHeld = false;
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(0, Point, Point, TSet<FKey>(),
				EKeys::LeftMouseButton, 0.0f, FModifierKeysState()));
		}
		Last = Point;
	}

	FAutoConsoleCommandWithWorldAndArgs TextsCommand(
		TEXT("apexsim.ui.Texts"),
		TEXT("Log every visible text with its centre in viewport pixels: apexsim.ui.Texts [filter]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			const FString Filter = FString::Join(Args, TEXT(" "));
			FVector2D Origin = FVector2D::ZeroVector;
			ViewportOrigin(Origin);
			for (const FFound& Found : VisibleTexts(World))
			{
				if (Filter.IsEmpty() || Found.Text.Contains(Filter))
				{
					UE_LOG(LogApexSim, Log, TEXT("apexsim.ui.Texts: (%4.0f, %4.0f) %s"),
						Found.Centre.X - Origin.X, Found.Centre.Y - Origin.Y, *Found.Text.Replace(TEXT("\n"), TEXT(" / ")));
				}
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ClickCommand(
		TEXT("apexsim.ui.Click"),
		TEXT("Click a visible text through Slate: apexsim.ui.Click <text> [#N] (Nth match, top to bottom, 0-based)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			TArray<FString> Words = Args;
			int32 Nth = 0;
			if (Words.Num() > 1 && Words.Last().StartsWith(TEXT("#")))
			{
				Nth = FCString::Atoi(*Words.Last().Mid(1));
				Words.Pop();
			}
			const FString Wanted = FString::Join(Words, TEXT(" "));
			int32 Seen = 0;
			for (const FFound& Found : VisibleTexts(World))
			{
				if (Found.Text.Equals(Wanted, ESearchCase::IgnoreCase) && Seen++ == Nth)
				{
					UE_LOG(LogApexSim, Log, TEXT("apexsim.ui.Click: '%s' #%d at (%.0f, %.0f)"), *Wanted, Nth, Found.Centre.X, Found.Centre.Y);
					Send(TEXT("down"), Found.Centre);
					Send(TEXT("up"), Found.Centre);
					return;
				}
			}
			UE_LOG(LogApexSim, Warning, TEXT("apexsim.ui.Click: no visible text '%s' #%d (%d matches)"), *Wanted, Nth, Seen);
		}));

	FAutoConsoleCommandWithWorldAndArgs MouseCommand(
		TEXT("apexsim.ui.Mouse"),
		TEXT("Drive the left mouse button at a viewport point: apexsim.ui.Mouse down|move|up X Y"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FVector2D Origin = FVector2D::ZeroVector;
			if (Args.Num() != 3 || !ViewportOrigin(Origin))
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.ui.Mouse down|move|up X Y"));
				return;
			}
			Send(*Args[0], Origin + FVector2D(FCString::Atod(*Args[1]), FCString::Atod(*Args[2])));
		}));
}
