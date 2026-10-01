#include "UI/ApexHudEditorWidget.h"

#include "ApexSim.h"
#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Rendering/DrawElements.h"
#include "TimerManager.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexHudWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"
#include "UObject/UObjectIterator.h"

using namespace ApexUI;

namespace
{
	const FName HudEdRow = TEXT("HudEditor.Row");
	const FName HudEdToggle = TEXT("HudEditor.Toggle");
	const FName HudEdSave = TEXT("HudEditor.Save");
	const FName HudEdCancel = TEXT("HudEditor.Cancel");
	const FName HudEdResetAll = TEXT("HudEditor.ResetAll");
	const FName HudEdResetOne = TEXT("HudEditor.ResetOne");
	const FName HudEdSmaller = TEXT("HudEditor.Smaller");
	const FName HudEdBigger = TEXT("HudEditor.Bigger");
	const FName HudEdHideCard = TEXT("HudEditor.HideCard");
	const FName HudEdShowCard = TEXT("HudEditor.ShowCard");

	/** How close, in HUD units, a dragged edge has to come to a guide to snap to it. */
	constexpr float HudEdSnapDistance = 8.0f;
	/** A size click or bracket press. */
	constexpr float HudEdScaleStep = 0.05f;
	/** How fast the pad's stick moves a panel, HUD units a second at full deflection. */
	constexpr float HudEdStickSpeed = 600.0f;
	/** Scripted steps (-ApexHudEditorSteps) run this far apart, so each is laid out before the next. */
	constexpr float HudEdStepInterval = 0.6f;
	constexpr float HudEdCardWidth = 540.0f;

	FAutoConsoleCommand HudEditCommand(TEXT("apexsim.hud.Edit"),
		TEXT("Open the HUD editor (as Settings > Gameplay > HUD layout does)."),
		FConsoleCommandDelegate::CreateLambda([]() {
			for (TObjectIterator<UApexRootWidget> It; It; ++It)
			{
				if (!It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && It->GetWorld())
				{
					It->OpenHudEditor();
				}
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs HudEditStepCommand(TEXT("apexsim.hud.EditStep"),
		TEXT("Drive the open HUD editor: select <id> | next | move <dx> <dy> | scale <step> | toggle <id> | reset | resetall | panel | save | cancel"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*) {
			const FString Step = FString::Join(Args, TEXT(" "));
			for (TObjectIterator<UApexHudEditorWidget> It; It; ++It)
			{
				if (It->IsOpen())
				{
					UE_LOG(LogApexSim, Display, TEXT("apexsim.hud.EditStep '%s'%s"), *Step,
						It->RunStep(Step) ? TEXT("") : TEXT(": not understood"));
				}
			}
		}));

	FString HudEdName(const FApexHudComponentDef& Component)
	{
		return Component.Name.IsEmpty() ? Component.Id : Component.Name;
	}

	FString HudEdPercent(float Scale)
	{
		return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Scale * 100.0f));
	}
}

// --- The frame layer ----------------------------------------------------------------

UApexHudFrameLayer::UApexHudFrameLayer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);

	FLinearColor Faint = FLinearColor::White;
	Faint.A = 0.35f;
	FrameBrush = MakeBrush(FLinearColor::Transparent, Faint, 1.0f);

	FLinearColor HoverFill = FLinearColor::White;
	HoverFill.A = 0.05f;
	HoverBrush = MakeBrush(HoverFill, Palette::TextPrimary, 1.0f);

	FLinearColor SelectedFill = Palette::Accent;
	SelectedFill.A = 0.08f;
	SelectedBrush = MakeBrush(SelectedFill, Palette::Accent, 2.0f);

	HandleBrush = MakeBrush(Palette::Accent);

	FLinearColor Guide = Palette::Accent;
	Guide.A = 0.85f;
	GuideBrush = MakeBrush(Guide);

	FLinearColor Grid = FLinearColor::White;
	Grid.A = 0.07f;
	GridBrush = MakeBrush(Grid);

	TagBrush = MakeBrush(Palette::Accent);
}

int32 UApexHudFrameLayer::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	LayerId = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	const UApexHudEditorWidget* Ed = Editor.Get();
	const UApexHudWidget* Hud = Ed ? Ed->GetHud() : nullptr;
	if (!Hud || !Ed->IsOpen())
	{
		return LayerId;
	}

	// HUD units to this layer's own, through the screen.
	auto ToLocal = [&](const FVector2D& HudPoint) { return AllottedGeometry.AbsoluteToLocal(Hud->HudToAbsolute(HudPoint)); };
	auto Box = [&](const FVector2D& HudTopLeft, const FVector2D& HudBottomRight, const FSlateBrush& Brush, int32 Layer)
	{
		const FVector2D TopLeft = ToLocal(HudTopLeft);
		const FVector2D Size = ToLocal(HudBottomRight) - TopLeft;
		// MakeBox colours by its own tint argument (white unless given), not
		// the brush's: widgets pass the brush's tint themselves, and so must this.
		FSlateDrawElement::MakeBox(OutDrawElements, Layer,
			AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(TopLeft)), &Brush,
			ESlateDrawEffect::None, Brush.TintColor.GetSpecifiedColor());
	};

	const FVector2D Screen = Hud->GetHudSize();
	const int32 GridLayer = LayerId + 1;
	const int32 FrameLayer = LayerId + 2;
	const int32 TopLayer = LayerId + 3;

	// The thirds: which one a panel's centre is in decides the corner it keeps to.
	for (int32 Third = 1; Third <= 2; ++Third)
	{
		const double X = Screen.X * Third / 3.0;
		const double Y = Screen.Y * Third / 3.0;
		Box(FVector2D(X, 0.0), FVector2D(X + 1.0, Screen.Y), GridBrush, GridLayer);
		Box(FVector2D(0.0, Y), FVector2D(Screen.X, Y + 1.0), GridBrush, GridLayer);
	}

	// What the drag is snapped to, across the whole screen.
	if (Ed->IsDragging())
	{
		const ApexHudPlace::FSnapLines& Lines = Ed->GetSnapLines();
		if (Lines.X.IsSet())
		{
			Box(FVector2D(Lines.X.GetValue(), 0.0), FVector2D(Lines.X.GetValue() + 1.0, Screen.Y), GuideBrush, TopLayer);
		}
		if (Lines.Y.IsSet())
		{
			Box(FVector2D(0.0, Lines.Y.GetValue()), FVector2D(Screen.X, Lines.Y.GetValue() + 1.0), GuideBrush, TopLayer);
		}
	}

	const TArray<FApexHudComponentDef>& Components = Hud->GetComponents();
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		FSlateRect Rect;
		if (!Hud->GetComponentRect(Index, Rect))
		{
			continue;
		}
		const bool bSelected = Index == Ed->GetSelected();
		const bool bHovered = Index == Ed->GetHovered();
		const FSlateBrush& Brush = bSelected ? SelectedBrush : bHovered ? HoverBrush : FrameBrush;
		Box(Rect.GetTopLeft(), Rect.GetBottomRight(), Brush, bSelected ? TopLayer : FrameLayer);

		if (!bSelected)
		{
			continue;
		}
		// The resize handle on the bottom-right corner.
		const FVector2D Corner = Rect.GetBottomRight();
		const float Half = UApexHudEditorWidget::HandleSize * 0.5f;
		Box(Corner - FVector2D(Half), Corner + FVector2D(Half), HandleBrush, TopLayer);

		// The name and size on a tag over the top-left corner, or under it at the top of the screen.
		const FString Tag = FString::Printf(TEXT("%s  %s"), *HudEdName(Components[Index]).ToUpper(),
			*HudEdPercent(Hud->GetLayout().ScaleOf(Components[Index].Id)));
		const FSlateFontInfo Font = Font::Mono(11.0f, 60);
		const FVector2D TextSize = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Tag, Font);
		const FVector2D TagSize = TextSize + FVector2D(16.0, 6.0);
		const double TagTop = Rect.Top - TagSize.Y - 4.0 >= 0.0 ? Rect.Top - TagSize.Y - 4.0 : Rect.Bottom + 4.0;
		const FVector2D TagTopLeft(Rect.Left, TagTop);
		Box(TagTopLeft, TagTopLeft + TagSize, TagBrush, TopLayer);
		const FVector2D TextAt = ToLocal(TagTopLeft + FVector2D(8.0, 3.0));
		FSlateDrawElement::MakeText(OutDrawElements, TopLayer + 1,
			AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(TextAt)),
			Tag, Font, ESlateDrawEffect::None, Palette::OnAccent);
	}
	return TopLayer + 1;
}

// --- The editor ----------------------------------------------------------------------

UApexHudEditorWidget::UApexHudEditorWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Collapsed);
	SetIsFocusable(true);
}

void UApexHudEditorWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildCard();
}

void UApexHudEditorWidget::BuildCard()
{
	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();

	// Something solid for the cursor to land on everywhere: a user widget's
	// own handlers only hear the clicks that hit one of its widgets, and the
	// rest of this one is layout and paint. Without it, a press on a panel
	// went through to the menu screen underneath.
	{
		UBorder* Catcher = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(FLinearColor::Transparent));
		Catcher->SetVisibility(ESlateVisibility::Visible);
		UOverlaySlot* CatcherSlot = Layers->AddChildToOverlay(Catcher);
		CatcherSlot->SetHorizontalAlignment(HAlign_Fill);
		CatcherSlot->SetVerticalAlignment(VAlign_Fill);
	}

	Frames = WidgetTree->ConstructWidget<UApexHudFrameLayer>();
	Frames->Editor = this;
	UOverlaySlot* FramesSlot = Layers->AddChildToOverlay(Frames);
	FramesSlot->SetHorizontalAlignment(HAlign_Fill);
	FramesSlot->SetVerticalAlignment(VAlign_Fill);

	auto MakeButton = [this](const FString& Label, FName Action, EApexButtonVariant Variant, float Height, float LabelSize,
		const FString& KeyCap = FString())
	{
		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.ActionId = Action;
		Spec.Variant = Variant;
		Spec.Height = Height;
		Spec.LabelSize = LabelSize;
		Spec.KeyCap = KeyCap;
		Spec.bCentreLabel = Variant != EApexButtonVariant::Panel;
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexHudEditorWidget::HandleButton);
		return Button;
	};

	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();

	// Title, and the way to get the card out from over a panel.
	{
		UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Header, MakeText(*WidgetTree, TEXT("HUD LAYOUT"), Font::Display(30.0f, 20), Palette::TextPrimary));
		AddH(Header, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
		AddH(Header, MakeButton(TEXT("Hide"), HudEdHideCard, EApexButtonVariant::Bare, 34.0f, 13.0f, TEXT("H")));
		AddV(Content, Header);
		UTextBlock* Hint = MakeText(*WidgetTree,
			TEXT("Drag a panel to move it and its corner to resize it; it snaps to the edges and the other panels (hold Shift to place it freely). "
				 "Tab picks a panel, the arrows nudge it, [ and ] size it, Del hides it."),
			Font::Body(12.0f), Palette::TextMuted);
		Hint->SetAutoWrapText(true);
		AddV(Content, Hint, FMargin(0.0f, 8.0f, 0.0f, 14.0f));
	}

	AddV(Content, MakeLabel(*WidgetTree, TEXT("Panels")), FMargin(0.0f, 0.0f, 0.0f, 8.0f));
	ListBox = WidgetTree->ConstructWidget<UVerticalBox>();
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	Scroll->AddChild(ListBox);
	USizeBox* ScrollSize = WidgetTree->ConstructWidget<USizeBox>();
	ScrollSize->SetMaxDesiredHeight(420.0f);
	ScrollSize->AddChild(Scroll);
	AddV(Content, ScrollSize);

	// The selected panel's size and reset.
	{
		AddV(Content, MakeDivider(*WidgetTree), FMargin(0.0f, 14.0f, 0.0f, 12.0f));
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		SelectedText = MakeText(*WidgetTree, FString(), Font::Body(15.0f, true), Palette::TextPrimary);
		AddH(Row, SelectedText, FMargin(), VAlign_Center, 1.0f);
		AddH(Row, MakeSized(*WidgetTree, MakeButton(TEXT("-"), HudEdSmaller, EApexButtonVariant::Ghost, 36.0f, 17.0f), 40.0f, 36.0f),
			FMargin(12.0f, 0.0f, 0.0f, 0.0f));
		ScaleText = MakeText(*WidgetTree, TEXT("100%"), Font::Mono(14.0f, 40), Palette::TextPrimary);
		ScaleText->SetJustification(ETextJustify::Center);
		AddH(Row, MakeSized(*WidgetTree, ScaleText, 64.0f, -1.0f));
		AddH(Row, MakeSized(*WidgetTree, MakeButton(TEXT("+"), HudEdBigger, EApexButtonVariant::Ghost, 36.0f, 17.0f), 40.0f, 36.0f));
		AddH(Row, MakeSized(*WidgetTree, MakeButton(TEXT("Reset"), HudEdResetOne, EApexButtonVariant::Ghost, 36.0f, 13.0f, TEXT("R")), 112.0f, 36.0f),
			FMargin(12.0f, 0.0f, 0.0f, 0.0f));
		AddV(Content, Row);
	}

	// Save, cancel, start over.
	{
		AddV(Content, MakeDivider(*WidgetTree), FMargin(0.0f, 12.0f, 0.0f, 14.0f));
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		AddH(Row, MakeSized(*WidgetTree, MakeButton(TEXT("Reset all"), HudEdResetAll, EApexButtonVariant::Ghost, 48.0f, 15.0f), 130.0f, 48.0f));
		AddH(Row, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);
		AddH(Row, MakeSized(*WidgetTree, MakeButton(TEXT("Cancel"), HudEdCancel, EApexButtonVariant::Ghost, 48.0f, 15.0f, TEXT("ESC")), 150.0f, 48.0f),
			FMargin(0.0f, 0.0f, 10.0f, 0.0f));
		AddH(Row, MakeSized(*WidgetTree, MakeButton(TEXT("Save"), HudEdSave, EApexButtonVariant::Primary, 48.0f, 15.0f, TEXT("ENTER")), 150.0f, 48.0f));
		AddV(Content, Row);
		StatusText = MakeText(*WidgetTree, FString(), Font::Body(11.0f), Palette::TextDisabled);
		StatusText->SetAutoWrapText(true);
		AddV(Content, StatusText, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
	}

	// The middle of the screen is the one place the HUD leaves empty.
	Card = MakeSized(*WidgetTree, MakeModalCard(*WidgetTree, Content, FMargin(26.0f, 22.0f)), HudEdCardWidth, -1.0f);
	UOverlaySlot* CardSlot = Layers->AddChildToOverlay(Card);
	CardSlot->SetHorizontalAlignment(HAlign_Center);
	CardSlot->SetVerticalAlignment(VAlign_Center);

	UApexButtonWidget* Show = MakeButton(TEXT("Show the HUD layout card"), HudEdShowCard, EApexButtonVariant::Primary, 40.0f, 13.0f, TEXT("H"));
	ShowCardButton = MakeSized(*WidgetTree, Show, 320.0f, 40.0f);
	ShowCardButton->SetVisibility(ESlateVisibility::Collapsed);
	UOverlaySlot* ShowSlot = Layers->AddChildToOverlay(ShowCardButton);
	ShowSlot->SetHorizontalAlignment(HAlign_Center);
	ShowSlot->SetVerticalAlignment(VAlign_Center);

	WidgetTree->RootWidget = Layers;
}

void UApexHudEditorWidget::RebuildList()
{
	UApexHudWidget* HudWidget = Hud.Get();
	if (!ListBox || !HudWidget)
	{
		return;
	}
	ListBox->ClearChildren();
	RowButtons.Reset();
	ToggleButtons.Reset();

	const TArray<FApexHudComponentDef>& Components = HudWidget->GetComponents();
	ListedCount = Components.Num();
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

		UApexButtonWidget* Name = WidgetTree->ConstructWidget<UApexButtonWidget>();
		FApexButtonSpec NameSpec;
		NameSpec.Label = HudEdName(Components[Index]);
		NameSpec.SubLabel = Components[Index].Description;
		NameSpec.ActionId = FName(HudEdRow, Index + 1);
		NameSpec.Variant = EApexButtonVariant::Panel;
		NameSpec.Height = Components[Index].Description.IsEmpty() ? 40.0f : 52.0f;
		NameSpec.LabelSize = 15.0f;
		Name->Setup(NameSpec);
		Name->OnActivated.AddDynamic(this, &UApexHudEditorWidget::HandleButton);
		AddH(Row, Name, FMargin(), VAlign_Fill, 1.0f);
		RowButtons.Add(Name);

		UApexButtonWidget* Toggle = WidgetTree->ConstructWidget<UApexButtonWidget>();
		FApexButtonSpec ToggleSpec;
		ToggleSpec.Label = TEXT("Hide");
		ToggleSpec.ActionId = FName(HudEdToggle, Index + 1);
		ToggleSpec.Variant = EApexButtonVariant::Ghost;
		ToggleSpec.Height = NameSpec.Height;
		ToggleSpec.LabelSize = 13.0f;
		ToggleSpec.bCentreLabel = true;
		Toggle->Setup(ToggleSpec);
		Toggle->OnActivated.AddDynamic(this, &UApexHudEditorWidget::HandleButton);
		AddH(Row, MakeSized(*WidgetTree, Toggle, 86.0f, -1.0f), FMargin(6.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill);
		ToggleButtons.Add(Toggle);

		AddV(ListBox, Row, FMargin(0.0f, Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f));
	}
}

void UApexHudEditorWidget::RefreshCard()
{
	UApexHudWidget* HudWidget = Hud.Get();
	if (!HudWidget)
	{
		return;
	}
	const TArray<FApexHudComponentDef>& Components = HudWidget->GetComponents();
	if (ListedCount != Components.Num())
	{
		RebuildList();
	}
	for (int32 Index = 0; Index < Components.Num() && Index < RowButtons.Num(); ++Index)
	{
		const bool bShown = HudWidget->IsComponentShown(Index);
		RowButtons[Index]->SetBadge(bShown ? TEXT("SHOWN") : TEXT("HIDDEN"), bShown ? Palette::Live : Palette::TextDisabled);
		RowButtons[Index]->SetSelected(Index == Selected);
		ToggleButtons[Index]->SetLabel(bShown ? TEXT("Hide") : TEXT("Show"));
	}

	if (SelectedText && ScaleText)
	{
		if (Components.IsValidIndex(Selected))
		{
			const FApexHudComponentDef& Component = Components[Selected];
			SelectedText->SetText(FText::FromString(HudWidget->IsComponentShown(Selected)
				? HudEdName(Component)
				: HudEdName(Component) + TEXT(" (hidden)")));
			ScaleText->SetText(FText::FromString(HudEdPercent(HudWidget->GetLayout().ScaleOf(Component.Id))));
		}
		else
		{
			SelectedText->SetText(FText::FromString(TEXT("Click a panel to pick it")));
			ScaleText->SetText(FText::FromString(TEXT("—")));
		}
	}
}

// --- Opening and closing ------------------------------------------------------------

void UApexHudEditorWidget::Open(UApexHudWidget* InHud)
{
	if (bOpen || !InHud)
	{
		return;
	}
	Hud = InHud;
	Original = InHud->GetLayout();
	bOpen = true;
	Selected = INDEX_NONE;
	Hovered = INDEX_NONE;
	Drag = EDrag::None;
	SetPanelHidden(false);

	InHud->BeginEditing();
	RebuildList();
	RefreshCard();
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("Saved to %s"),
			*FPaths::ConvertRelativePathToFull(InHud->GetLayoutFile()))));
	}

	SetVisibility(ESlateVisibility::Visible);
	FocusDefault();

	// An unattended run (screenshots, the editor's own checks) drives it by steps.
	FString Steps;
	if (FParse::Value(FCommandLine::Get(), TEXT("ApexHudEditorSteps="), Steps, /*bShouldStopOnSeparator*/ false))
	{
		Steps.ParseIntoArray(PendingSteps, TEXT(";"), true);
		StepCountdown = HudEdStepInterval;
	}
	UE_LOG(LogApexSim, Log, TEXT("HUD editor open (%d component(s))"), InHud->GetComponents().Num());
}

void UApexHudEditorWidget::Close(bool bSave)
{
	if (!bOpen)
	{
		return;
	}
	if (Drag != EDrag::None)
	{
		EndDrag();
	}
	if (UApexHudWidget* HudWidget = Hud.Get())
	{
		if (bSave)
		{
			const FString File = HudWidget->GetLayoutFile();
			if (HudWidget->GetLayout().Save(File))
			{
				UE_LOG(LogApexSim, Log, TEXT("HUD layout saved to %s"), *File);
			}
			else
			{
				UE_LOG(LogApexSim, Warning, TEXT("HUD layout could not be written to %s"), *File);
			}
		}
		else
		{
			HudWidget->SetLayout(Original);
		}
		HudWidget->EndEditing();
	}
	ApexUiAudio::Play(this, bSave ? EApexUiSound::Accept : EApexUiSound::Back);
	bOpen = false;
	PendingSteps.Reset();
	SetVisibility(ESlateVisibility::Collapsed);
	OnClosed.Broadcast();
}

void UApexHudEditorWidget::FocusDefault()
{
	// The editor itself takes the keys, not a button on its card: the arrows
	// move the panel, and Enter saves whatever was clicked last.
	if (!ApexNav::Focus(this))
	{
		SetKeyboardFocus();
	}
}

void UApexHudEditorWidget::SetPanelHidden(bool bHidden)
{
	bPanelHidden = bHidden;
	if (Card)
	{
		Card->SetVisibility(bHidden ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	if (ShowCardButton)
	{
		ShowCardButton->SetVisibility(bHidden ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UApexHudEditorWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!bOpen)
	{
		return;
	}
	RefreshCard();

	// The stick moves the panel smoothly, faster the further it is pushed.
	if (Stick.Size() > 0.2)
	{
		MoveSelected(FVector2D(Stick.X, -Stick.Y) * HudEdStickSpeed * InDeltaTime);
	}

	if (!PendingSteps.IsEmpty())
	{
		StepCountdown -= InDeltaTime;
		if (StepCountdown <= 0.0f)
		{
			StepCountdown = HudEdStepInterval;
			const FString Step = PendingSteps[0];
			PendingSteps.RemoveAt(0);
			// Not from here: a widget ticks inside Slate's paint, while the
			// frame's hit-test grid is half built, so a synthesised click found
			// only what had been painted so far (the menu under the editor).
			// The world's timer runs before Slate does, as a real click would.
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, Step]()
				{
					UE_LOG(LogApexSim, Log, TEXT("HUD editor step '%s'%s"), *Step, RunStep(Step) ? TEXT("") : TEXT(": not understood"));
				}));
			}
		}
	}
}

// --- Editing ------------------------------------------------------------------------

bool UApexHudEditorWidget::BeginChange(FApexHudPlacement& OutPlacement, FSlateRect& OutRect)
{
	UApexHudWidget* HudWidget = Hud.Get();
	if (!HudWidget || !HudWidget->GetComponents().IsValidIndex(Selected) || !HudWidget->IsComponentShown(Selected))
	{
		return false;
	}
	// Measured before pinning: a pin that rebuilds the HUD leaves no geometry
	// until the next frame, and it does not move anything.
	const bool bHasRect = HudWidget->GetComponentRect(Selected, OutRect);
	HudWidget->PinAll();
	const FApexHudPlacement* Placement = HudWidget->GetLayout().Find(HudWidget->GetComponents()[Selected].Id);
	if (!Placement || !Placement->bPinned)
	{
		return false;
	}
	OutPlacement = *Placement;
	if (!bHasRect)
	{
		OutRect = FSlateRect();
	}
	return true;
}

bool UApexHudEditorWidget::SelectById(const FString& Id)
{
	const UApexHudWidget* HudWidget = Hud.Get();
	if (!HudWidget)
	{
		return false;
	}
	const int32 Index = HudWidget->GetComponents().IndexOfByPredicate(
		[&Id](const FApexHudComponentDef& Component) { return Component.Id == Id; });
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Selected = Index;
	return true;
}

void UApexHudEditorWidget::SelectNext(int32 Step)
{
	const UApexHudWidget* HudWidget = Hud.Get();
	const int32 Count = HudWidget ? HudWidget->GetComponents().Num() : 0;
	if (Count == 0)
	{
		return;
	}
	// Every component, hidden ones too: the way back for one hidden with the pad.
	Selected = Selected == INDEX_NONE ? (Step > 0 ? 0 : Count - 1) : (Selected + Step + Count) % Count;
	ApexUiAudio::Play(this, EApexUiSound::Move);
}

void UApexHudEditorWidget::MoveSelected(const FVector2D& Delta)
{
	FApexHudPlacement Placement;
	FSlateRect Rect;
	if (!BeginChange(Placement, Rect))
	{
		return;
	}
	Placement.Position += Delta;
	// Never off the screen: a panel nobody can see is a panel nobody can grab.
	if (Rect.GetSize().X > 0.0)
	{
		const FVector2D Screen = Hud->GetHudSize();
		const FVector2D Size = Rect.GetSize();
		const FVector2D TopLeft = ApexHudPlace::TopLeft(Placement, Size, Screen);
		const FVector2D Held(FMath::Clamp(TopLeft.X, 0.0, FMath::Max(0.0, Screen.X - Size.X)),
			FMath::Clamp(TopLeft.Y, 0.0, FMath::Max(0.0, Screen.Y - Size.Y)));
		Placement.Position += Held - TopLeft;
	}
	Hud->PlaceComponent(Selected, Placement);
}

void UApexHudEditorWidget::ScaleSelected(float Step)
{
	FApexHudPlacement Placement;
	FSlateRect Rect;
	if (!BeginChange(Placement, Rect))
	{
		return;
	}
	const float NewScale = ApexHudPlace::ClampScale(Placement.Scale + Step);
	if (NewScale == Placement.Scale)
	{
		return;
	}
	// Grown about its anchor corner, which is what the position is measured
	// from, so a panel at the bottom grows upward rather than off the screen.
	// (Dragging the handle holds the top left instead: that is the corner the
	// hand is not on.)
	Placement.Scale = NewScale;
	Hud->PlaceComponent(Selected, Placement);
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
}

void UApexHudEditorWidget::ToggleComponent(int32 Index)
{
	UApexHudWidget* HudWidget = Hud.Get();
	if (!HudWidget || !HudWidget->GetComponents().IsValidIndex(Index))
	{
		return;
	}
	const FString& Id = HudWidget->GetComponents()[Index].Id;
	const FApexHudPlacement* Existing = HudWidget->GetLayout().Find(Id);
	FApexHudPlacement Placement = Existing ? *Existing : FApexHudPlacement();
	Placement.bEnabled = !HudWidget->IsComponentShown(Index);
	HudWidget->PlaceComponent(Index, Placement);
	Selected = Index;
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
}

void UApexHudEditorWidget::ResetSelected()
{
	UApexHudWidget* HudWidget = Hud.Get();
	if (!HudWidget || !HudWidget->GetComponents().IsValidIndex(Selected))
	{
		return;
	}
	// Back to its component.json's place and size; a panel the player added
	// stays added.
	const FApexHudComponentDef& Component = HudWidget->GetComponents()[Selected];
	FApexHudLayout Layout = HudWidget->GetLayout();
	const bool bShown = HudWidget->IsComponentShown(Selected);
	if (bShown == Component.bDefaultEnabled)
	{
		Layout.Components.Remove(Component.Id);
	}
	else
	{
		FApexHudPlacement Placement;
		Placement.bEnabled = bShown;
		Layout.Components.Add(Component.Id, Placement);
	}
	HudWidget->SetLayout(Layout);
	ApexUiAudio::Play(this, EApexUiSound::Adjust);
}

void UApexHudEditorWidget::ResetAll()
{
	if (UApexHudWidget* HudWidget = Hud.Get())
	{
		HudWidget->SetLayout(FApexHudLayout());
		ApexUiAudio::Play(this, EApexUiSound::Adjust);
	}
}

bool UApexHudEditorWidget::RunStep(const FString& InStep)
{
	TArray<FString> Words;
	InStep.TrimStartAndEnd().ParseIntoArrayWS(Words);
	if (Words.IsEmpty())
	{
		return false;
	}
	const FString Verb = Words[0].ToLower();
	if (Verb == TEXT("select") && Words.Num() == 2)
	{
		return SelectById(Words[1]);
	}
	if (Verb == TEXT("next"))
	{
		SelectNext(1);
		return true;
	}
	if (Verb == TEXT("move") && Words.Num() == 3)
	{
		MoveSelected(FVector2D(FCString::Atod(*Words[1]), FCString::Atod(*Words[2])));
		return true;
	}
	if (Verb == TEXT("scale") && Words.Num() == 2)
	{
		ScaleSelected(FCString::Atof(*Words[1]));
		return true;
	}
	if (Verb == TEXT("toggle") && Words.Num() == 2)
	{
		if (!SelectById(Words[1]))
		{
			return false;
		}
		ToggleComponent(Selected);
		return true;
	}
	if (Verb == TEXT("reset"))
	{
		ResetSelected();
		return true;
	}
	if (Verb == TEXT("resetall"))
	{
		ResetAll();
		return true;
	}
	if (Verb == TEXT("panel"))
	{
		SetPanelHidden(!bPanelHidden);
		return true;
	}
	// The mouse, through Slate's own input pipeline rather than around it.
	if (Verb == TEXT("grab") && (Words.Num() == 2 || Words.Num() == 3) && Hud.IsValid())
	{
		const int32 Index = Hud->GetComponents().IndexOfByPredicate(
			[&Words](const FApexHudComponentDef& Component) { return Component.Id == Words[1]; });
		FSlateRect Rect;
		if (Index == INDEX_NONE || !Hud->GetComponentRect(Index, Rect))
		{
			return false;
		}
		const bool bCorner = Words.Num() == 3 && Words[2] == TEXT("corner");
		// The corner grab needs the panel selected first, as a hand would have it.
		Selected = bCorner ? Index : Selected;
		ScriptCursor = Hud->HudToAbsolute(bCorner ? FVector2D(Rect.GetBottomRight()) : FVector2D(Rect.GetCenter()));
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.SetCursorPos(ScriptCursor);
		Slate.ProcessMouseMoveEvent(FPointerEvent(0, ScriptCursor, ScriptCursor, TSet<FKey>(), FKey(), 0.0f, FModifierKeysState()));
		Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(0, ScriptCursor, ScriptCursor,
			TSet<FKey>({EKeys::LeftMouseButton}), EKeys::LeftMouseButton, 0.0f, FModifierKeysState()));
		return true;
	}
	if (Verb == TEXT("dragby") && Words.Num() == 3 && Hud.IsValid())
	{
		const FVector2D From = ScriptCursor;
		const FVector2D HudFrom = Hud->AbsoluteToHud(From);
		ScriptCursor = Hud->HudToAbsolute(HudFrom + FVector2D(FCString::Atod(*Words[1]), FCString::Atod(*Words[2])));
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.SetCursorPos(ScriptCursor);
		Slate.ProcessMouseMoveEvent(FPointerEvent(0, ScriptCursor, From, TSet<FKey>({EKeys::LeftMouseButton}), FKey(), 0.0f,
			FModifierKeysState()));
		return true;
	}
	if (Verb == TEXT("release"))
	{
		FSlateApplication::Get().ProcessMouseButtonUpEvent(FPointerEvent(0, ScriptCursor, ScriptCursor, TSet<FKey>(),
			EKeys::LeftMouseButton, 0.0f, FModifierKeysState()));
		return true;
	}
	if (Verb == TEXT("save") || Verb == TEXT("cancel"))
	{
		Close(Verb == TEXT("save"));
		return true;
	}
	return false;
}

// --- Buttons ------------------------------------------------------------------------

void UApexHudEditorWidget::HandleButton(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}
	const FName Action = Button->GetActionId();
	const FString Plain = Action.GetPlainNameString();
	if (Plain == HudEdRow.ToString())
	{
		Selected = Action.GetNumber() - 1;
	}
	else if (Plain == HudEdToggle.ToString())
	{
		ToggleComponent(Action.GetNumber() - 1);
	}
	else if (Action == HudEdSave || Action == HudEdCancel)
	{
		Close(Action == HudEdSave);
		return;
	}
	else if (Action == HudEdResetAll)
	{
		ResetAll();
	}
	else if (Action == HudEdResetOne)
	{
		ResetSelected();
	}
	else if (Action == HudEdSmaller || Action == HudEdBigger)
	{
		ScaleSelected(Action == HudEdBigger ? HudEdScaleStep : -HudEdScaleStep);
	}
	else if (Action == HudEdHideCard || Action == HudEdShowCard)
	{
		SetPanelHidden(Action == HudEdHideCard);
	}
	// The keys go back to the editor whatever was clicked.
	FocusDefault();
}

// --- Mouse --------------------------------------------------------------------------

bool UApexHudEditorWidget::IsOverCard(const FVector2D& ScreenPosition) const
{
	const UWidget* Shown = bPanelHidden ? ShowCardButton.Get() : Card.Get();
	return Shown && Shown->GetCachedGeometry().IsUnderLocation(ScreenPosition);
}

int32 UApexHudEditorWidget::HitTest(const FVector2D& HudPoint) const
{
	const UApexHudWidget* HudWidget = Hud.Get();
	if (!HudWidget)
	{
		return INDEX_NONE;
	}
	int32 Best = INDEX_NONE;
	double BestArea = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < HudWidget->GetComponents().Num(); ++Index)
	{
		FSlateRect Rect;
		if (HudWidget->GetComponentRect(Index, Rect) && Rect.ContainsPoint(HudPoint))
		{
			// The smallest under the cursor: a badge on top of a big panel stays reachable.
			const double Area = Rect.GetArea();
			if (Area < BestArea)
			{
				BestArea = Area;
				Best = Index;
			}
		}
	}
	return Best;
}

bool UApexHudEditorWidget::IsOverHandle(const FVector2D& HudPoint) const
{
	const UApexHudWidget* HudWidget = Hud.Get();
	FSlateRect Rect;
	if (!HudWidget || !HudWidget->GetComponentRect(Selected, Rect))
	{
		return false;
	}
	// A little more generous than the square drawn, which is small.
	const FVector2D Corner = Rect.GetBottomRight();
	return FVector2D::Distance(HudPoint, Corner) <= HandleSize;
}

FReply UApexHudEditorWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	UApexHudWidget* HudWidget = Hud.Get();
	if (!bOpen || !HudWidget || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	}
	const FVector2D Screen = InMouseEvent.GetScreenSpacePosition();
	if (IsOverCard(Screen))
	{
		return FReply::Unhandled();
	}
	FReply Reply = FReply::Handled().SetUserFocus(TakeWidget(), EFocusCause::Mouse);

	const FVector2D Point = HudWidget->AbsoluteToHud(Screen);
	EDrag Mode = EDrag::Move;
	if (Selected != INDEX_NONE && IsOverHandle(Point))
	{
		Mode = EDrag::Resize;
	}
	else
	{
		Selected = HitTest(Point);
		if (Selected == INDEX_NONE)
		{
			return Reply;
		}
	}

	// Every other panel, measured now, is what this one snaps to.
	DragOthers.Reset();
	for (int32 Index = 0; Index < HudWidget->GetComponents().Num(); ++Index)
	{
		FSlateRect Other;
		if (Index != Selected && HudWidget->GetComponentRect(Index, Other))
		{
			DragOthers.Add(Other);
		}
	}
	if (!BeginChange(DragStartPlacement, DragStartRect) || DragStartRect.GetSize().X <= 0.0)
	{
		return Reply;
	}
	Drag = Mode;
	DragStartMouse = Point;
	SnapLines = ApexHudPlace::FSnapLines();
	return Reply.CaptureMouse(TakeWidget());
}

FReply UApexHudEditorWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	UApexHudWidget* HudWidget = Hud.Get();
	if (!bOpen || !HudWidget)
	{
		return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
	}
	const FVector2D Screen = InMouseEvent.GetScreenSpacePosition();
	const FVector2D Point = HudWidget->AbsoluteToHud(Screen);
	if (Drag == EDrag::None)
	{
		Hovered = IsOverCard(Screen) ? INDEX_NONE : HitTest(Point);
		return FReply::Unhandled();
	}

	const FVector2D ScreenSize = HudWidget->GetHudSize();
	const FVector2D Delta = Point - DragStartMouse;
	FApexHudPlacement Placement = DragStartPlacement;
	if (Drag == EDrag::Move)
	{
		FSlateRect Moved = DragStartRect.OffsetBy(Delta);
		// Kept on the screen before it is snapped, so a guide at the edge still catches it.
		const FVector2D Size = Moved.GetSize();
		const FVector2D Held(FMath::Clamp(static_cast<double>(Moved.Left), 0.0, FMath::Max(0.0, ScreenSize.X - Size.X)),
			FMath::Clamp(static_cast<double>(Moved.Top), 0.0, FMath::Max(0.0, ScreenSize.Y - Size.Y)));
		Moved = FSlateRect(Held, Held + Size);
		if (InMouseEvent.IsShiftDown())
		{
			SnapLines = ApexHudPlace::FSnapLines();
		}
		else
		{
			Moved = ApexHudPlace::Snap(Moved, ScreenSize, DragOthers, HudEdSnapDistance, SnapLines);
		}
		Placement.Position += Moved.GetTopLeft() - DragStartRect.GetTopLeft();
	}
	else
	{
		// The corner follows the cursor: the scale is the mean of how far it
		// has stretched each way, so a diagonal drag tracks it closely.
		const FVector2D Size = DragStartRect.GetSize();
		const double Stretch = ((Size.X + Delta.X) / Size.X + (Size.Y + Delta.Y) / Size.Y) * 0.5;
		Placement.Scale = ApexHudPlace::ClampScale(static_cast<float>(DragStartPlacement.Scale * Stretch));
		const FVector2D NewSize = Size * (Placement.Scale / DragStartPlacement.Scale);
		Placement.Position = DragStartRect.GetTopLeft() + NewSize * Placement.Anchor - ScreenSize * Placement.Anchor;
	}
	HudWidget->PlaceComponent(Selected, Placement);
	return FReply::Handled();
}

FReply UApexHudEditorWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Drag == EDrag::None || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
	}
	EndDrag();
	return FReply::Handled().ReleaseMouseCapture();
}

void UApexHudEditorWidget::EndDrag()
{
	UApexHudWidget* HudWidget = Hud.Get();
	const EDrag Ended = Drag;
	Drag = EDrag::None;
	SnapLines = ApexHudPlace::FSnapLines();
	if (!HudWidget || Ended == EDrag::None || !HudWidget->GetComponents().IsValidIndex(Selected))
	{
		return;
	}
	// Re-anchored where it was dropped, so a panel dragged to the bottom right
	// keeps to the bottom right on any screen. Nothing moves.
	const FApexHudPlacement* Placement = HudWidget->GetLayout().Find(HudWidget->GetComponents()[Selected].Id);
	if (!Placement || !Placement->bPinned)
	{
		return;
	}
	const FVector2D ScreenSize = HudWidget->GetHudSize();
	const FVector2D Size = DragStartRect.GetSize() * (Placement->Scale / DragStartPlacement.Scale);
	const FVector2D TopLeft = ApexHudPlace::TopLeft(*Placement, Size, ScreenSize);
	HudWidget->PlaceComponent(Selected,
		ApexHudPlace::PinRect(FSlateRect(TopLeft, TopLeft + Size), ScreenSize, Placement->Scale, Placement->bEnabled));
}

FCursorReply UApexHudEditorWidget::NativeOnCursorQuery(const FGeometry& InGeometry, const FPointerEvent& InCursorEvent)
{
	const UApexHudWidget* HudWidget = Hud.Get();
	if (!bOpen || !HudWidget || IsOverCard(InCursorEvent.GetScreenSpacePosition()))
	{
		return Super::NativeOnCursorQuery(InGeometry, InCursorEvent);
	}
	if (Drag == EDrag::Resize)
	{
		return FCursorReply::Cursor(EMouseCursor::ResizeSouthEast);
	}
	if (Drag == EDrag::Move)
	{
		return FCursorReply::Cursor(EMouseCursor::GrabHandClosed);
	}
	const FVector2D Point = HudWidget->AbsoluteToHud(InCursorEvent.GetScreenSpacePosition());
	if (IsOverHandle(Point))
	{
		return FCursorReply::Cursor(EMouseCursor::ResizeSouthEast);
	}
	return HitTest(Point) != INDEX_NONE ? FCursorReply::Cursor(EMouseCursor::GrabHand) : FCursorReply::Unhandled();
}

// --- Keys and pad -------------------------------------------------------------------

FReply UApexHudEditorWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (!bOpen)
	{
		return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
	}
	const FKey Key = InKeyEvent.GetKey();
	const bool bShift = InKeyEvent.IsShiftDown();

	if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right)
	{
		if (Drag != EDrag::None)
		{
			// A drag in progress is put back, the editor stays.
			Drag = EDrag::None;
			SnapLines = ApexHudPlace::FSnapLines();
			if (Hud.IsValid())
			{
				Hud->PlaceComponent(Selected, DragStartPlacement);
			}
			return FReply::Handled().ReleaseMouseCapture();
		}
		Close(false);
		return FReply::Handled();
	}
	if (Key == EKeys::Enter || Key == EKeys::Gamepad_Special_Right)
	{
		Close(true);
		return FReply::Handled();
	}
	if (Key == EKeys::Tab || Key == EKeys::Gamepad_RightShoulder || Key == EKeys::Gamepad_LeftShoulder)
	{
		SelectNext(Key == EKeys::Gamepad_LeftShoulder || (Key == EKeys::Tab && bShift) ? -1 : 1);
		return FReply::Handled();
	}

	// Nudges: a unit on the keyboard (ten with Shift), four on the D-pad.
	const double Nudge = Key.IsGamepadKey() ? 4.0 : bShift ? 10.0 : 1.0;
	FVector2D Delta = FVector2D::ZeroVector;
	if (Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left) { Delta.X = -Nudge; }
	else if (Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Right) { Delta.X = Nudge; }
	else if (Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up) { Delta.Y = -Nudge; }
	else if (Key == EKeys::Down || Key == EKeys::Gamepad_DPad_Down) { Delta.Y = Nudge; }
	if (!Delta.IsZero())
	{
		MoveSelected(Delta);
		return FReply::Handled();
	}

	if (Key == EKeys::LeftBracket || Key == EKeys::Hyphen || Key == EKeys::Subtract || Key == EKeys::Gamepad_LeftTrigger)
	{
		ScaleSelected(-HudEdScaleStep);
		return FReply::Handled();
	}
	if (Key == EKeys::RightBracket || Key == EKeys::Equals || Key == EKeys::Add || Key == EKeys::Gamepad_RightTrigger)
	{
		ScaleSelected(HudEdScaleStep);
		return FReply::Handled();
	}
	if (Key == EKeys::Delete || Key == EKeys::Gamepad_FaceButton_Top)
	{
		ToggleComponent(Selected);
		return FReply::Handled();
	}
	if (Key == EKeys::R || Key == EKeys::Gamepad_FaceButton_Left)
	{
		ResetSelected();
		return FReply::Handled();
	}
	if (Key == EKeys::H || Key == EKeys::Gamepad_Special_Left)
	{
		SetPanelHidden(!bPanelHidden);
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UApexHudEditorWidget::NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent)
{
	if (!bOpen)
	{
		return Super::NativeOnAnalogValueChanged(InGeometry, InAnalogEvent);
	}
	const FKey Key = InAnalogEvent.GetKey();
	if (Key == EKeys::Gamepad_LeftX)
	{
		Stick.X = InAnalogEvent.GetAnalogValue();
	}
	else if (Key == EKeys::Gamepad_LeftY)
	{
		Stick.Y = InAnalogEvent.GetAnalogValue();
	}
	return FReply::Handled();
}
