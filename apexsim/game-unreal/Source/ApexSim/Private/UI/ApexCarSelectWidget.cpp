#include "UI/ApexCarSelectWidget.h"

#include "ApexCarPreviewStage.h"
#include "Cars/ApexCarContentSubsystem.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScaleBox.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "TimerManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Race/ApexCarLivery.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionCarSelectBack(TEXT("__back"));
	const FName ActionSetup(TEXT("__setup"));
	const FName ActionLivery(TEXT("__livery"));
	const FName ActionConfirm(TEXT("__confirm"));
	const FName ActionEditLivery(TEXT("__livery_edit"));
	const FName ActionEditorPaint(TEXT("__editor_paint"));
	const FName ActionEditorAccent(TEXT("__editor_accent"));
	const FName ActionEditorNumber(TEXT("__editor_number"));
	const FName ActionEditorSave(TEXT("__editor_save"));
	const FName ActionEditorCancel(TEXT("__editor_cancel"));

	constexpr float EditorWidth = 440.0f;
	/** A draft's number runs 0..99, or none (-1: the car keeps its own logo). */
	constexpr int32 MaxNumber = 99;

	constexpr float ListWidth = 420.0f;
	/** Watts per kilowatt-hour of marketing: kW to the horsepower everyone quotes. */
	constexpr float HorsepowerPerKilowatt = 1.34102f;
}

UApexCarSelectWidget::UApexCarSelectWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexCarSelectWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexCarSelectWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexCarSelectWidget::HandleLobbyStateUpdated);
	}
}

void UApexCarSelectWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexCarSelectWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexCarSelectWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	// The stage renders whether or not anyone is looking, so it only spins while
	// this screen is up.
	if (AApexCarPreviewStage* Stage = AApexCarPreviewStage::Find(this))
	{
		Stage->SetTurntableEnabled(true);

		if (StageImage)
		{
			if (UTextureRenderTarget2D* RenderTarget = Stage->GetPreviewRenderTarget())
			{
				FSlateBrush Brush = StageImage->GetBrush();
				Brush.SetResourceObject(RenderTarget);
				Brush.ImageSize = Stage->GetPreviewSize();
				StageImage->SetBrush(Brush);
				StageImage->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
		}
	}
	else if (StageImage)
	{
		StageImage->SetVisibility(ESlateVisibility::Hidden);
		UE_LOG(LogApexSim, Warning, TEXT("No AApexCarPreviewStage in the level; the car preview is disabled"));
	}

	RebuildList();

	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		if (Flow->HasPendingCar())
		{
			SelectCar(Flow->GetPendingCarId());
		}
	}
	if (SelectedCarId.IsEmpty() && VisibleRows.Num() > 0)
	{
		SelectCar(VisibleRows[0]->GetActionId().ToString());
	}
	// Focus lands a tick later, by way of the root's RequestFocusDefault.
}

void UApexCarSelectWidget::OnScreenDeactivated()
{
	Super::OnScreenDeactivated();

	if (AApexCarPreviewStage* Stage = AApexCarPreviewStage::Find(this))
	{
		Stage->SetTurntableEnabled(false);
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexCarSelectWidget::BuildLayout()
{
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Page, BuildHeader());

	CarListBox = WidgetTree->ConstructWidget<UVerticalBox>();
	CarListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	CarListScroll->AddChild(CarListBox);

	// The car list comes from the server, so before the first LobbyState (or
	// while the server is down) the list is empty and needs to say why.
	EmptyText = ApexUI::MakeText(
		*WidgetTree,
		TEXT("No cars yet — the list fills in as soon as the server is reachable."),
		ApexUI::Font::Body(15.0f),
		ApexUI::Palette::TextMuted);
	EmptyText->SetAutoWrapText(true);
	EmptyText->SetVisibility(ESlateVisibility::Collapsed);

	UVerticalBox* ListColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(ListColumn, EmptyText, FMargin(0.0f, 4.0f));
	ApexUI::AddV(ListColumn, CarListScroll, FMargin(), HAlign_Fill, 1.0f);

	UBorder* ListPanel = ApexUI::MakePanel(
		*WidgetTree,
		ListColumn,
		FMargin(20.0f, 18.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Columns, ApexUI::MakeSized(*WidgetTree, ListPanel, ListWidth, -1.0f), FMargin(), VAlign_Fill);
	ApexUI::AddH(Columns, ApexUI::MakeDivider(*WidgetTree, true), FMargin(), VAlign_Fill);
	ApexUI::AddH(Columns, BuildStage(), FMargin(), VAlign_Fill, 1.0f);

	ApexUI::AddV(Page, Columns, FMargin(), HAlign_Fill, 1.0f);

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
}

UWidget* UApexCarSelectWidget::BuildHeader()
{
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.KeyCap = TEXT("Esc");
	BackSpec.bKeyCapLeading = true;
	BackSpec.Sound = EApexUiSound::Back;
	BackSpec.Variant = EApexButtonVariant::Bare;
	BackSpec.LabelSize = 15.0f;
	BackSpec.ActionId = ActionCarSelectBack;

	HeaderBackButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	HeaderBackButton->Setup(BackSpec);
	HeaderBackButton->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);

	ChipRow = WidgetTree->ConstructWidget<UHorizontalBox>();

	return ApexUI::MakeScreenHeader(*WidgetTree, HeaderBackButton, TEXT("Garage"), ChipRow);
}

UWidget* UApexCarSelectWidget::BuildStage()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	StageImage = WidgetTree->ConstructWidget<UImage>();
	// The frame is much wider than the capture; fitting rather than filling
	// keeps the car its own shape, with the frame colour either side.
	UScaleBox* StageFit = WidgetTree->ConstructWidget<UScaleBox>();
	StageFit->SetStretch(EStretch::ScaleToFit);
	StageFit->AddChild(StageImage);
	UBorder* StageFrame = ApexUI::MakePanel(
		*WidgetTree,
		StageFit,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));
	UOverlay* StageStack = WidgetTree->ConstructWidget<UOverlay>();
	if (UOverlaySlot* FrameSlot = StageStack->AddChildToOverlay(StageFrame))
	{
		FrameSlot->SetHorizontalAlignment(HAlign_Fill);
		FrameSlot->SetVerticalAlignment(VAlign_Fill);
	}
	if (UOverlaySlot* EditorSlot = StageStack->AddChildToOverlay(BuildLiveryEditor()))
	{
		EditorSlot->SetHorizontalAlignment(HAlign_Right);
		EditorSlot->SetVerticalAlignment(VAlign_Center);
		EditorSlot->SetPadding(FMargin(16.0f));
	}
	ApexUI::AddV(Column, StageStack, FMargin(), HAlign_Fill, 1.0f);

	// Name and actions.
	UVerticalBox* NameStack = WidgetTree->ConstructWidget<UVerticalBox>();
	EyebrowText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 160), ApexUI::Palette::Accent);
	ApexUI::AddV(NameStack, EyebrowText);
	NameText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(34.0f), ApexUI::Palette::TextPrimary);
	ApexUI::AddV(NameStack, NameText, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	UHorizontalBox* ActionRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(ActionRow, NameStack, FMargin(), VAlign_Center, 1.0f);

	// The paint scheme: click (or Left / Right in the list) steps through them.
	FApexButtonSpec LiverySpec;
	LiverySpec.Label = TEXT("Livery");
	LiverySpec.KeyCap = TEXT("◀ ▶");
	LiverySpec.Variant = EApexButtonVariant::Ghost;
	LiverySpec.LabelSize = 17.0f;
	LiverySpec.Height = 56.0f;
	LiverySpec.ActionId = ActionLivery;
	// A pad changes the livery with left and right on a car's row (the key
	// cap says so), so the button is the mouse's: a pad that could focus it
	// would find it a dead end, every direction on the rows being taken.
	LiverySpec.bMouseOnly = true;

	LiveryButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	LiveryButton->Setup(LiverySpec);
	LiveryButton->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);
	// The livery's own picture, when it ships one, just before its name.
	LiveryPreview = WidgetTree->ConstructWidget<UImage>();
	LiveryPreview->SetVisibility(ESlateVisibility::Collapsed);
	ApexUI::AddH(ActionRow, ApexUI::MakeSized(*WidgetTree, LiveryPreview, 96.0f, 54.0f), FMargin(16.0f, 0.0f, 0.0f, 0.0f), VAlign_Center);
	ApexUI::AddH(ActionRow, ApexUI::MakeSized(*WidgetTree, LiveryButton, 300.0f, -1.0f), FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	// The livery editor. A pad opens it with Y on a car's row (the key cap
	// says E, the keyboard's), so the button, like the livery's, is the mouse's.
	FApexButtonSpec EditSpec;
	EditSpec.Label = TEXT("Edit");
	EditSpec.KeyCap = TEXT("E");
	EditSpec.Variant = EApexButtonVariant::Ghost;
	EditSpec.LabelSize = 17.0f;
	EditSpec.Height = 56.0f;
	EditSpec.ActionId = ActionEditLivery;
	EditSpec.bMouseOnly = true;
	EditLiveryButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	EditLiveryButton->Setup(EditSpec);
	EditLiveryButton->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);
	ApexUI::AddH(ActionRow, ApexUI::MakeSized(*WidgetTree, EditLiveryButton, 130.0f, -1.0f), FMargin(10.0f, 0.0f, 0.0f, 0.0f));

	// No setup system exists yet, so the button says so rather than doing nothing.
	FApexButtonSpec SetupSpec;
	SetupSpec.Label = TEXT("Setup sheet");
	SetupSpec.Badge = TEXT("Locked");
	SetupSpec.Variant = EApexButtonVariant::Locked;
	SetupSpec.LabelSize = 17.0f;
	SetupSpec.Height = 56.0f;
	SetupSpec.ActionId = ActionSetup;

	UApexButtonWidget* SetupButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	SetupButton->Setup(SetupSpec);
	ApexUI::AddH(ActionRow, ApexUI::MakeSized(*WidgetTree, SetupButton, 200.0f, -1.0f), FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	// A click on a row selects the car to look at; this (or a double-click on
	// the row) chooses it. The mouse's: a key or pad A on a row chooses at
	// once, as moving through the list already describes each car.
	FApexButtonSpec ConfirmSpec;
	ConfirmSpec.Label = TEXT("Confirm");
	ConfirmSpec.Variant = EApexButtonVariant::Primary;
	ConfirmSpec.bCentreLabel = true;
	ConfirmSpec.LabelSize = 17.0f;
	ConfirmSpec.Height = 56.0f;
	ConfirmSpec.ActionId = ActionConfirm;
	ConfirmSpec.bMouseOnly = true;
	ConfirmButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	ConfirmButton->Setup(ConfirmSpec);
	ConfirmButton->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);
	ApexUI::AddH(ActionRow, ApexUI::MakeSized(*WidgetTree, ConfirmButton, 200.0f, -1.0f), FMargin(16.0f, 0.0f, 0.0f, 0.0f));

	ApexUI::AddV(Column, ActionRow, FMargin(0.0f, 22.0f, 0.0f, 0.0f));

	// Four numbers, three of them measured against the rest of the field.
	// The out-params are raw pointers because TObjectPtr members cannot bind to
	// a pointer reference; they are stored on the widget straight after.
	UTextBlock* PowerText = nullptr;
	UTextBlock* WeightText = nullptr;
	UTextBlock* RatioText = nullptr;
	UTextBlock* ClassText = nullptr;
	UProgressBar* PowerBarPtr = nullptr;
	UProgressBar* WeightBarPtr = nullptr;
	UProgressBar* RatioBarPtr = nullptr;
	UProgressBar* UnusedBar = nullptr;

	UHorizontalBox* Stats = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Stats, ApexUI::MakeStatBar(*WidgetTree, TEXT("Power"), PowerText, PowerBarPtr), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Stats, ApexUI::MakeStatBar(*WidgetTree, TEXT("Weight"), WeightText, WeightBarPtr), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddH(Stats, ApexUI::MakeStatBar(*WidgetTree, TEXT("Power / weight"), RatioText, RatioBarPtr), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddH(Stats, ApexUI::MakeStatBar(*WidgetTree, TEXT("Class"), ClassText, UnusedBar), FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);

	PowerValue = PowerText;
	WeightValue = WeightText;
	RatioValue = RatioText;
	ClassValue = ClassText;
	PowerBar = PowerBarPtr;
	WeightBar = WeightBarPtr;
	RatioBar = RatioBarPtr;

	if (UnusedBar)
	{
		// Class is a name, not a quantity; the bar under it would be noise.
		UnusedBar->SetVisibility(ESlateVisibility::Hidden);
	}

	ApexUI::AddV(Column, Stats, FMargin(0.0f, 18.0f, 0.0f, 0.0f));

	return ApexUI::MakePanel(
		*WidgetTree,
		Column,
		FMargin(28.0f, 20.0f, ApexUI::Metrics::PageGutter, 26.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
}

UWidget* UApexCarSelectWidget::BuildLiveryEditor()
{
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Box, ApexUI::MakeLabel(*WidgetTree, TEXT("Livery editor"), ApexUI::Palette::Accent));

	auto Stepper = [this, Box](const TCHAR* Label, FName Id)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.KeyCap = TEXT("◀ ▶");
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.LabelSize = 17.0f;
		Spec.Height = 52.0f;
		Spec.ActionId = Id;
		UApexButtonWidget* Row = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Row->Setup(Spec);
		Row->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);
		ApexUI::AddV(Box, Row, FMargin(0.0f, 10.0f, 0.0f, 0.0f));
		return Row;
	};
	EditorPaintRow = Stepper(TEXT("Paint"), ActionEditorPaint);
	EditorAccentRow = Stepper(TEXT("Accent"), ActionEditorAccent);
	EditorNumberRow = Stepper(TEXT("Number"), ActionEditorNumber);

	ApexUI::AddV(Box, ApexUI::MakeLabel(*WidgetTree, TEXT("Name")), FMargin(0.0f, 16.0f, 0.0f, 0.0f));
	EditorNameBox = ApexUI::MakeSearchBox(*WidgetTree, TEXT("The paint and number, until you type one"));
	EditorNameBox->OnTextChanged.AddDynamic(this, &UApexCarSelectWidget::HandleEditorNameChanged);
	ApexUI::AddV(Box, EditorNameBox, FMargin(0.0f, 6.0f, 0.0f, 0.0f));

	EditorNote = ApexUI::MakeText(*WidgetTree,
		TEXT("Kept in cars/custom/liveries, listed as yours. In a session the other drivers see it too."),
		ApexUI::Font::Body(13.0f), ApexUI::Palette::TextMuted);
	EditorNote->SetAutoWrapText(true);
	ApexUI::AddV(Box, EditorNote, FMargin(0.0f, 12.0f, 0.0f, 0.0f));

	FApexButtonSpec SaveSpec;
	SaveSpec.Label = TEXT("Save");
	SaveSpec.Variant = EApexButtonVariant::Primary;
	SaveSpec.bCentreLabel = true;
	SaveSpec.LabelSize = 17.0f;
	SaveSpec.Height = 52.0f;
	SaveSpec.ActionId = ActionEditorSave;
	EditorSaveButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	EditorSaveButton->Setup(SaveSpec);
	EditorSaveButton->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);

	FApexButtonSpec CancelSpec;
	CancelSpec.Label = TEXT("Cancel");
	CancelSpec.KeyCap = TEXT("Esc");
	CancelSpec.Sound = EApexUiSound::Back;
	CancelSpec.Variant = EApexButtonVariant::Ghost;
	CancelSpec.bCentreLabel = true;
	CancelSpec.LabelSize = 17.0f;
	CancelSpec.Height = 52.0f;
	CancelSpec.ActionId = ActionEditorCancel;
	EditorCancelButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	EditorCancelButton->Setup(CancelSpec);
	EditorCancelButton->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Buttons, EditorSaveButton, FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Buttons, EditorCancelButton, FMargin(10.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(Box, Buttons, FMargin(0.0f, 16.0f, 0.0f, 0.0f));

	EditorRows = {EditorPaintRow, EditorAccentRow, EditorNumberRow, EditorSaveButton, EditorCancelButton};

	LiveryEditorPanel = ApexUI::MakePanel(*WidgetTree, Box, FMargin(22.0f, 18.0f),
		ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));
	LiveryEditorPanel->SetVisibility(ESlateVisibility::Collapsed);
	return ApexUI::MakeSized(*WidgetTree, LiveryEditorPanel, EditorWidth, -1.0f);
}

// ---------------------------------------------------------------------------
// The livery editor
// ---------------------------------------------------------------------------

bool UApexCarSelectWidget::IsLiveryEditorOpen() const
{
	return LiveryEditorPanel && LiveryEditorPanel->GetVisibility() != ESlateVisibility::Collapsed;
}

FString UApexCarSelectWidget::SelectedCarDir() const
{
	const UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get();
	return Content && !SelectedCarId.IsEmpty() ? Content->CarDirectory(SelectedCarId) : FString();
}

bool UApexCarSelectWidget::OpenLiveryEditor()
{
	if (!LiveryEditorPanel || SelectedCarId.IsEmpty())
	{
		return false;
	}
	if (SelectedCarDir().IsEmpty())
	{
		ShowToast(TEXT("This car has no folder on this machine to keep a livery in"), true);
		return false;
	}
	Draft = FApexLiveryDraft();
	Draft.Number = 1;
	bDraftNamed = false;
	LiveryEditorPanel->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	RefreshLiveryEditor();
	ApexNav::Focus(EditorPaintRow);
	return true;
}

void UApexCarSelectWidget::CloseLiveryEditor()
{
	if (!IsLiveryEditorOpen())
	{
		return;
	}
	LiveryEditorPanel->SetVisibility(ESlateVisibility::Collapsed);
	// The turntable back in the livery the garage has picked.
	UpdatePreviewStage();
	FocusRow(FocusedRowIndex);
}

void UApexCarSelectWidget::StepEditor(UApexButtonWidget* Row, int32 Step)
{
	const int32 Swatches = ApexLiveryEditor::Swatches().Num();
	auto Wrap = [](int32 Value, int32 Count) { return ((Value % Count) + Count) % Count; };
	if (Row == EditorPaintRow)
	{
		Draft.Paint = Wrap(Draft.Paint + Step, Swatches);
	}
	else if (Row == EditorAccentRow)
	{
		Draft.Accent = Wrap(Draft.Accent + Step, Swatches);
	}
	else if (Row == EditorNumberRow)
	{
		// -1 (none) and 0..99 in a ring.
		Draft.Number = Wrap(Draft.Number + 1 + Step, MaxNumber + 2) - 1;
	}
	else
	{
		return;
	}
	RefreshLiveryEditor();
}

void UApexCarSelectWidget::HandleEditorNameChanged(const FText& Text)
{
	// SetText below raises this too; only the player's typing names the draft.
	if (Text.ToString() != Draft.Name)
	{
		Draft.Name = ApexLiveryEditor::CleanName(Text.ToString());
		bDraftNamed = !Draft.Name.IsEmpty();
	}
}

void UApexCarSelectWidget::RefreshLiveryEditor()
{
	const TConstArrayView<ApexLiveryEditor::FSwatch> All = ApexLiveryEditor::Swatches();
	auto Badge = [&All](UApexButtonWidget* Row, int32 Index)
	{
		if (Row)
		{
			const ApexLiveryEditor::FSwatch& Swatch = All[((Index % All.Num()) + All.Num()) % All.Num()];
			// The name in its own colour: the swatch itself.
			Row->SetBadge(FString(Swatch.Name).ToUpper(), FLinearColor::FromSRGBColor(Swatch.Srgb));
		}
	};
	Badge(EditorPaintRow, Draft.Paint);
	Badge(EditorAccentRow, Draft.Accent);
	if (EditorNumberRow)
	{
		EditorNumberRow->SetBadge(Draft.Number >= 0 ? FString::FromInt(Draft.Number) : FString(TEXT("NONE")),
			ApexUI::Palette::TextPrimary);
	}
	if (!bDraftNamed)
	{
		Draft.Name = ApexLiveryEditor::AutoName(Draft);
		if (EditorNameBox)
		{
			EditorNameBox->SetText(FText::FromString(Draft.Name));
		}
	}

	// The draft on the turntable: its colours, and its number on the logo,
	// drawn once per number and plate colour into Saved/ApexSim/LiveryEditor.
	FApexCarLivery Preview;
	Preview.Name = Draft.Name;
	Preview.Paint = ApexLiveryEditor::Linear(Draft.Paint);
	Preview.Accent = ApexLiveryEditor::Linear(Draft.Accent);
	if (Draft.Number >= 0)
	{
		const FString Key = FString::Printf(TEXT("%d_%d"), Draft.Number, Draft.Accent);
		if (const FString* Known = DraftLogos.Find(Key))
		{
			Preview.RuntimeLogo = *Known;
		}
		else
		{
			TArray<uint8> Png;
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ApexSim"), TEXT("LiveryEditor"),
				FString::Printf(TEXT("logo_%s.png"), *Key));
			if (ApexLiveryEditor::RenderNumberLogo(Draft.Number, Preview.Accent, Png) && FFileHelper::SaveArrayToFile(Png, *Path))
			{
				Preview.RuntimeLogo = Path;
			}
			DraftLogos.Add(Key, Preview.RuntimeLogo);
		}
	}
	if (AApexCarPreviewStage* Stage = AApexCarPreviewStage::Find(this))
	{
		Stage->SetCarLivery(&Preview);
	}
}

void UApexCarSelectWidget::SaveLiveryEditor()
{
	const FString CarDir = SelectedCarDir();
	TArray<uint8> Logo;
	if (Draft.Number >= 0 && !ApexLiveryEditor::RenderNumberLogo(Draft.Number, ApexLiveryEditor::Linear(Draft.Accent), Logo))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Livery editor: no renderer to draw the number; the livery keeps the car's logo"));
	}
	FString Folder;
	FString Error;
	if (!ApexLiveryEditor::Save(CarDir, Draft, Logo, Folder, Error))
	{
		ShowToast(FString::Printf(TEXT("The livery was not saved: %s"), *Error), true);
		return;
	}
	UE_LOG(LogApexSim, Log, TEXT("Livery editor: %s saved in %s"), *Draft.Name, *Folder);
	if (UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get())
	{
		Content->RescanRows();
	}
	// The new livery picked: the last of the player's own with its name.
	FApexCarCatalogRow Row;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (Flow && Flow->GetCarCatalogRow(SelectedCarId, Row))
	{
		const FString Saved = ApexLiveryEditor::CleanName(Draft.Name);
		for (int32 Index = Row.Liveries.Num() - 1; Index >= 0; --Index)
		{
			if (Row.Liveries[Index].bOwn && Row.Liveries[Index].Name == Saved
				&& FPaths::IsUnderDirectory(Row.Liveries[Index].RuntimeLogo.IsEmpty() ? Folder : Row.Liveries[Index].RuntimeLogo, Folder))
			{
				SelectedLivery = Index + 1;
				break;
			}
		}
	}
	LiveryEditorPanel->SetVisibility(ESlateVisibility::Collapsed);
	RefreshLiveryButton();
	UpdatePreviewStage();
	FocusRow(FocusedRowIndex);
	ShowToast(FString::Printf(TEXT("Saved: %s (yours)"), *ApexLiveryEditor::CleanName(Draft.Name)));
}

bool UApexCarSelectWidget::HandleBack()
{
	if (IsLiveryEditorOpen())
	{
		CloseLiveryEditor();
		return true;
	}
	return Super::HandleBack();
}

FReply UApexCarSelectWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const bool bTyping = EditorNameBox && EditorNameBox->HasKeyboardFocus();
	if (!IsLiveryEditorOpen() && !bTyping && !InKeyEvent.IsRepeat() && (Key == EKeys::E || Key == EKeys::Gamepad_FaceButton_Top))
	{
		if (OpenLiveryEditor())
		{
			return FReply::Handled();
		}
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

float UApexCarSelectWidget::GetPowerHp(const FApexCarCatalogRow& Row)
{
	return Row.MaxPowerKw * HorsepowerPerKilowatt;
}

void UApexCarSelectWidget::RebuildList(bool bForce)
{
	const UApexNetSubsystem* Net = GetNet();
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !CarListBox)
	{
		return;
	}

	const TArray<FApexCarConfigSummary>& Cars = Net->GetCachedLobbyState().CarConfigs;

	if (EmptyText)
	{
		EmptyText->SetVisibility(Cars.Num() == 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}

	TArray<FString> IncomingIds;
	IncomingIds.Reserve(Cars.Num());
	for (const FApexCarConfigSummary& Car : Cars)
	{
		IncomingIds.Add(Car.Id);
	}
	if (!bForce && IncomingIds == BuiltCarIds)
	{
		return;
	}
	BuiltCarIds = MoveTemp(IncomingIds);

	CarRows.Reset();
	CarListBox->ClearChildren();

	for (const FApexCarConfigSummary& Car : Cars)
	{
		FApexCarCatalogRow Row;
		const bool bHasRow = Flow && Flow->GetCarCatalogRow(Car.Id, Row);

		TArray<FString> SpecParts;
		const float Hp = bHasRow ? GetPowerHp(Row) : 0.0f;
		if (Hp > 0.0f)
		{
			SpecParts.Add(FString::Printf(TEXT("%.0f hp"), Hp));
		}
		const float Mass = Car.MassKg > 0.0f ? Car.MassKg : (bHasRow ? Row.MassKg : 0.0f);
		if (Mass > 0.0f)
		{
			SpecParts.Add(FString::Printf(TEXT("%.0f kg"), Mass));
		}
		if (bHasRow && Row.ModelYear > 0)
		{
			SpecParts.Add(FString::FromInt(Row.ModelYear));
		}

		FApexButtonSpec Spec;
		Spec.Label = bHasRow && !Row.DisplayName.IsEmpty() ? Row.DisplayName : Car.Name;
		Spec.SubLabel = FString::Join(SpecParts, TEXT(" · "));
		Spec.Badge = bHasRow ? ApexCatalog::DisplayClass(Row.CarClass) : FString();
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.LabelSize = 18.0f;
		Spec.Height = 76.0f;
		// The row's action id is the car id: one handler, no lookup table.
		Spec.ActionId = FName(*Car.Id);

		UApexButtonWidget* RowWidget = WidgetTree->ConstructWidget<UApexButtonWidget>();
		RowWidget->Setup(Spec);
		RowWidget->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleRowActivated);
		CarRows.Add(RowWidget);
	}

	RebuildFilterChips();
	ApplyFilter();

	// The screen is usually opened before the first lobby snapshot arrives, so
	// the rows here are new objects and the selection made against the old,
	// empty list has to be applied again — otherwise nothing is outlined and the
	// stat bars, which are relative to the field, stay at zero.
	if (SelectedCarId.IsEmpty())
	{
		if (Flow && Flow->HasPendingCar())
		{
			SelectedCarId = Flow->GetPendingCarId();
		}
		else if (VisibleRows.Num() > 0)
		{
			SelectedCarId = VisibleRows[0]->GetActionId().ToString();
		}
	}
	if (!SelectedCarId.IsEmpty())
	{
		SelectCar(SelectedCarId);
		RequestRowFocus();
	}
}

void UApexCarSelectWidget::RequestRowFocus()
{
	// The list is rebuilt from lobby snapshots whether or not this screen is
	// showing; only the screen in front may move focus.
	if (!IsActiveScreen())
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				if (IsActiveScreen())
				{
					FocusRow(FocusedRowIndex);
				}
			}));
	}
}

void UApexCarSelectWidget::RebuildFilterChips()
{
	if (!ChipRow)
	{
		return;
	}

	ChipRow->ClearChildren();
	FilterChips.Reset();

	TArray<FString> Classes;
	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		for (const FString& Id : BuiltCarIds)
		{
			FApexCarCatalogRow Row;
			if (Flow->GetCarCatalogRow(Id, Row) && !Row.CarClass.IsEmpty())
			{
				Classes.AddUnique(ApexCatalog::DisplayClass(Row.CarClass));
			}
		}
	}
	Classes.Sort();

	auto AddChip = [this](const FString& Key, const FString& Label)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = ActiveFilter == Key ? EApexButtonVariant::Primary : EApexButtonVariant::Ghost;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 14.0f;
		Spec.Height = 34.0f;
		Spec.ActionId = FName(*Key);

		UApexButtonWidget* Chip = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Chip->Setup(Spec);
		Chip->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);

		ApexUI::AddH(ChipRow, ApexUI::MakeSized(*WidgetTree, Chip, -1.0f, 34.0f), FMargin(6.0f, 0.0f, 0.0f, 0.0f));
		FilterChips.Add(Chip);
	};

	AddChip(FString(), FString::Printf(TEXT("All %d"), CarRows.Num()));
	for (const FString& CarClass : Classes)
	{
		AddChip(CarClass, CarClass);
	}
}

void UApexCarSelectWidget::ApplyFilter()
{
	if (!CarListBox)
	{
		return;
	}

	const UApexMenuFlowSubsystem* Flow = GetFlow();

	VisibleRows.Reset();
	CarListBox->ClearChildren();

	for (UApexButtonWidget* RowWidget : CarRows)
	{
		if (!RowWidget)
		{
			continue;
		}

		bool bMatches = ActiveFilter.IsEmpty();
		if (!bMatches && Flow)
		{
			FApexCarCatalogRow Row;
			bMatches = Flow->GetCarCatalogRow(RowWidget->GetActionId().ToString(), Row)
				&& ApexCatalog::DisplayClass(Row.CarClass).Equals(ActiveFilter, ESearchCase::IgnoreCase);
		}

		if (!bMatches)
		{
			continue;
		}

		ApexUI::AddV(CarListBox, RowWidget, FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		VisibleRows.Add(RowWidget);
	}

	FocusedRowIndex = FMath::Clamp(FocusedRowIndex, 0, FMath::Max(0, VisibleRows.Num() - 1));
}

void UApexCarSelectWidget::SelectCar(const FString& CarId)
{
	if (!SelectedCarId.Equals(CarId, ESearchCase::IgnoreCase))
	{
		if (IsLiveryEditorOpen() && LiveryEditorPanel)
		{
			LiveryEditorPanel->SetVisibility(ESlateVisibility::Collapsed);
		}
		// Another car starts in its works livery, unless it is the car the
		// player already drives, which comes back in the livery they picked.
		SelectedLivery = 0;
		const UApexMenuFlowSubsystem* Flow = GetFlow();
		const UApexNetSubsystem* Net = GetNet();
		if (Flow && Net && Flow->HasPendingCar() && Flow->GetPendingCarId().Equals(CarId, ESearchCase::IgnoreCase))
		{
			SelectedLivery = Net->GetPendingLivery();
		}
	}
	SelectedCarId = CarId;

	for (int32 Index = 0; Index < VisibleRows.Num(); ++Index)
	{
		const bool bIsSelected = VisibleRows[Index]->GetActionId().ToString().Equals(CarId, ESearchCase::IgnoreCase);
		VisibleRows[Index]->SetSelected(bIsSelected);
		if (bIsSelected)
		{
			FocusedRowIndex = Index;
		}
	}
	for (UApexButtonWidget* RowWidget : CarRows)
	{
		if (RowWidget && !VisibleRows.Contains(RowWidget))
		{
			RowWidget->SetSelected(false);
		}
	}

	RefreshDetail();
	UpdatePreviewStage();
}

void UApexCarSelectWidget::RefreshDetail()
{
	const UApexNetSubsystem* Net = GetNet();
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Net || !Flow)
	{
		return;
	}

	FApexCarCatalogRow Row;
	const bool bHasRow = Flow->GetCarCatalogRow(SelectedCarId, Row);

	FApexCarConfigSummary Summary;
	const bool bHasSummary = Net->FindCarById(SelectedCarId, Summary);

	const FString Name = bHasRow && !Row.DisplayName.IsEmpty()
		? Row.DisplayName
		: (bHasSummary ? Summary.Name : FString());

	if (NameText)
	{
		NameText->SetText(FText::FromString(Name.IsEmpty() ? TEXT("No car selected") : Name));
	}

	if (EyebrowText)
	{
		TArray<FString> Parts;
		if (bHasRow)
		{
			if (!Row.CarClass.IsEmpty())            { Parts.Add(ApexCatalog::DisplayClass(Row.CarClass).ToUpper()); }
			if (!Row.Brand.IsEmpty())               { Parts.Add(Row.Brand.ToUpper()); }
			if (!Row.ManufacturerCountry.IsEmpty()) { Parts.Add(Row.ManufacturerCountry.ToUpper()); }
		}
		EyebrowText->SetText(FText::FromString(FString::Join(Parts, TEXT(" · "))));
	}

	// The bars are shares of the best in the current field, so they need the
	// field's extremes first.
	float MaxHp = 0.0f;
	float MaxMass = 0.0f;
	float MaxRatio = 0.0f;
	for (const FString& Id : BuiltCarIds)
	{
		FApexCarCatalogRow Other;
		const bool bOtherRow = Flow->GetCarCatalogRow(Id, Other);
		FApexCarConfigSummary OtherSummary;
		const bool bOtherSummary = Net->FindCarById(Id, OtherSummary);

		const float OtherHp = bOtherRow ? GetPowerHp(Other) : 0.0f;
		const float OtherMass = bOtherSummary && OtherSummary.MassKg > 0.0f
			? OtherSummary.MassKg
			: (bOtherRow ? Other.MassKg : 0.0f);

		MaxHp = FMath::Max(MaxHp, OtherHp);
		MaxMass = FMath::Max(MaxMass, OtherMass);
		if (OtherMass > 0.0f)
		{
			MaxRatio = FMath::Max(MaxRatio, OtherHp / (OtherMass / 1000.0f));
		}
	}

	const float Hp = bHasRow ? GetPowerHp(Row) : 0.0f;
	const float Mass = bHasSummary && Summary.MassKg > 0.0f ? Summary.MassKg : (bHasRow ? Row.MassKg : 0.0f);
	const float Ratio = Mass > 0.0f ? Hp / (Mass / 1000.0f) : 0.0f;

	if (PowerValue)
	{
		PowerValue->SetText(FText::FromString(Hp > 0.0f ? FString::Printf(TEXT("%.0f hp"), Hp) : TEXT("—")));
	}
	if (WeightValue)
	{
		WeightValue->SetText(FText::FromString(Mass > 0.0f ? FString::Printf(TEXT("%.0f kg"), Mass) : TEXT("—")));
	}
	if (RatioValue)
	{
		RatioValue->SetText(FText::FromString(Ratio > 0.0f ? FString::Printf(TEXT("%.0f hp/t"), Ratio) : TEXT("—")));
	}
	if (ClassValue)
	{
		ClassValue->SetText(FText::FromString(bHasRow && !Row.CarClass.IsEmpty() ? ApexCatalog::DisplayClass(Row.CarClass) : TEXT("—")));
	}

	if (PowerBar)  { PowerBar->SetPercent(MaxHp > 0.0f ? Hp / MaxHp : 0.0f); }
	if (WeightBar) { WeightBar->SetPercent(MaxMass > 0.0f ? Mass / MaxMass : 0.0f); }
	if (RatioBar)  { RatioBar->SetPercent(MaxRatio > 0.0f ? Ratio / MaxRatio : 0.0f); }

	RefreshLiveryButton();
}

void UApexCarSelectWidget::UpdatePreviewStage()
{
	AApexCarPreviewStage* Stage = AApexCarPreviewStage::Find(this);
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Stage || !Flow)
	{
		return;
	}

	FApexCarCatalogRow Row;
	if (!Flow->GetCarCatalogRow(SelectedCarId, Row))
	{
		Stage->SetCarWheels(FApexWheelSpec());
		Stage->SetCarDrsFlap(FApexDrsFlapSpec());
		Stage->SetCarMesh(nullptr);
		return;
	}

	Stage->SetPreviewTransform(Row.PreviewOffset, Row.PreviewRotation, Row.PreviewScale);
	Stage->SetCarWheels(Row.Wheels);
	Stage->SetCarDrsFlap(Row.DrsFlap);
	Stage->SetCarMesh(ApexCarContent::LoadBody(Row));
	Stage->SetCarLivery(ApexLivery::Find(Row, SelectedLivery));
}

void UApexCarSelectWidget::CycleLivery(int32 Step)
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	FApexCarCatalogRow Row;
	if (!Flow || !Flow->GetCarCatalogRow(SelectedCarId, Row) || Row.Liveries.Num() == 0)
	{
		return;
	}
	const int32 Count = Row.Liveries.Num() + 1;
	SelectedLivery = ((SelectedLivery + Step) % Count + Count) % Count;
	RefreshLiveryButton();
	UpdatePreviewStage();
}

void UApexCarSelectWidget::RefreshLiveryButton()
{
	if (!LiveryButton)
	{
		return;
	}
	if (EditLiveryButton)
	{
		EditLiveryButton->SetVisibility(SelectedCarDir().IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	FApexCarCatalogRow Row;
	const bool bHasRow = Flow && Flow->GetCarCatalogRow(SelectedCarId, Row);
	if (!bHasRow || Row.Liveries.Num() == 0)
	{
		SelectedLivery = 0;
		LiveryButton->SetVisibility(ESlateVisibility::Collapsed);
		RefreshLiveryPreview(nullptr);
		return;
	}
	SelectedLivery = FMath::Clamp(SelectedLivery, 0, Row.Liveries.Num());
	LiveryButton->SetVisibility(ESlateVisibility::Visible);
	LiveryButton->SetLabel(ApexLivery::DisplayName(Row, SelectedLivery));
	LiveryButton->SetBadge(FString::Printf(TEXT("%d / %d"), SelectedLivery + 1, Row.Liveries.Num() + 1),
		ApexUI::Palette::TextMuted);
	RefreshLiveryPreview(ApexLivery::Find(Row, SelectedLivery));
}

void UApexCarSelectWidget::RefreshLiveryPreview(const FApexCarLivery* Livery)
{
	if (!LiveryPreview)
	{
		return;
	}
	UTexture2D* Texture = nullptr;
	if (Livery && !Livery->RuntimePreview.IsEmpty())
	{
		if (const TObjectPtr<UTexture2D>* Known = LiveryPreviewTextures.Find(Livery->RuntimePreview))
		{
			Texture = *Known;
		}
		else
		{
			// A file that will not read is remembered too, as nothing.
			Texture = FImageUtils::ImportFileAsTexture2D(Livery->RuntimePreview);
			if (!Texture)
			{
				UE_LOG(LogApexSim, Warning, TEXT("Livery preview %s could not be read"), *Livery->RuntimePreview);
			}
			LiveryPreviewTextures.Add(Livery->RuntimePreview, Texture);
		}
	}
	if (Texture)
	{
		LiveryPreview->SetBrushFromTexture(Texture);
	}
	LiveryPreview->SetVisibility(Texture ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexCarSelectWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RebuildList();
}

void UApexCarSelectWidget::HandleRowActivated(UApexButtonWidget* Row)
{
	// Moving through the list describes a car, so a key or pad A on a row is
	// the choice (a confirm button meant a second trip across the page with a
	// pad). A click only selects, so the mouse can look at the car and change
	// its livery first; Confirm or a double-click chooses it. As on the track
	// screen.
	if (!Row)
	{
		return;
	}
	SelectCar(Row->GetActionId().ToString());
	if (Row->GetLastActivation() != EApexActivation::Click)
	{
		ConfirmSelection();
	}
}

void UApexCarSelectWidget::ConfirmSelection()
{
	if (SelectedCarId.IsEmpty())
	{
		return;
	}
	if (UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		Flow->SetPendingCar(SelectedCarId);
	}
	// The server tracks the selection too, and rejects it if the player is in
	// a session that has already started.
	if (UApexNetSubsystem* Net = GetNet())
	{
		FApexCarCatalogRow Row;
		const UApexMenuFlowSubsystem* LiveryFlow = GetFlow();
		const bool bOwn = LiveryFlow && LiveryFlow->GetCarCatalogRow(SelectedCarId, Row)
			&& ApexLivery::ForServer(Row, SelectedLivery) != SelectedLivery;
		Net->SetPendingLivery(SelectedLivery, bOwn);
		Net->SelectCar(SelectedCarId);
	}
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ReplaceScreen(Root->ScreenAfterCarSelect);
	}
}

void UApexCarSelectWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const FName Id = Button->GetActionId();

	if (Id == ActionCarSelectBack)
	{
		GoBack();
		return;
	}
	if (Id == ActionLivery)
	{
		CycleLivery(+1);
		return;
	}
	if (Id == ActionConfirm)
	{
		ConfirmSelection();
		return;
	}
	if (Id == ActionEditLivery)
	{
		OpenLiveryEditor();
		return;
	}
	if (Id == ActionEditorPaint || Id == ActionEditorAccent || Id == ActionEditorNumber)
	{
		StepEditor(Button, +1);
		return;
	}
	if (Id == ActionEditorSave)
	{
		SaveLiveryEditor();
		return;
	}
	if (Id == ActionEditorCancel)
	{
		CloseLiveryEditor();
		return;
	}

	// Otherwise a class chip.
	ActiveFilter = Id.IsNone() ? FString() : Id.ToString();
	RebuildFilterChips();
	ApplyFilter();
	if (!SelectedCarId.IsEmpty())
	{
		SelectCar(SelectedCarId);
	}
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void UApexCarSelectWidget::FocusDefault()
{
	// The list, or — before the server has sent any cars — the way out.
	if (!FocusRow(FocusedRowIndex) && !ApexNav::Focus(HeaderBackButton))
	{
		Super::FocusDefault();
	}
}

bool UApexCarSelectWidget::FocusRow(int32 Index)
{
	if (VisibleRows.Num() == 0)
	{
		return false;
	}

	FocusedRowIndex = FMath::Clamp(Index, 0, VisibleRows.Num() - 1);
	if (!ApexNav::Focus(VisibleRows[FocusedRowIndex]))
	{
		return false;
	}

	// Moving through the list is how you browse the garage: each row describes
	// itself on the right as it is reached.
	SelectCar(VisibleRows[FocusedRowIndex]->GetActionId().ToString());
	return true;
}

bool UApexCarSelectWidget::FocusChip()
{
	for (UApexButtonWidget* Chip : FilterChips)
	{
		if (Chip && Chip->GetActionId().ToString() == ActiveFilter && ApexNav::Focus(Chip))
		{
			return true;
		}
	}
	return FilterChips.Num() > 0 && ApexNav::Focus(FilterChips[0]);
}

bool UApexCarSelectWidget::HandleNavigation(EUINavigation Direction, UWidget* Source)
{
	// The editor holds the pad while it is up: up and down through its rows,
	// left and right step a row's value or move between Save and Cancel.
	const int32 EditorAt = ApexNav::IndexOf(EditorRows, Source);
	if (EditorAt != INDEX_NONE)
	{
		constexpr int32 SaveAt = 3;
		constexpr int32 CancelAt = 4;
		switch (Direction)
		{
		case EUINavigation::Up:
		case EUINavigation::Previous:
			if (EditorAt > 0)
			{
				ApexNav::Focus(EditorRows[EditorAt == CancelAt ? SaveAt - 1 : EditorAt - 1]);
			}
			return true;
		case EUINavigation::Down:
		case EUINavigation::Next:
			if (EditorAt < SaveAt)
			{
				ApexNav::Focus(EditorRows[EditorAt + 1]);
			}
			return true;
		case EUINavigation::Left:
		case EUINavigation::Right:
			if (EditorAt < SaveAt)
			{
				StepEditor(EditorRows[EditorAt], Direction == EUINavigation::Right ? +1 : -1);
			}
			else
			{
				ApexNav::Focus(EditorRows[Direction == EUINavigation::Right ? CancelAt : SaveAt]);
			}
			return true;
		default:
			return true;
		}
	}
	if (IsLiveryEditorOpen() && Source == EditorNameBox)
	{
		// Typing: left and right move the caret; up and down leave the box.
		if (Direction == EUINavigation::Up)
		{
			return ApexNav::Focus(EditorNumberRow);
		}
		if (Direction == EUINavigation::Down)
		{
			return ApexNav::Focus(EditorSaveButton);
		}
		return false;
	}

	const int32 RowAt = ApexNav::IndexOf(VisibleRows, Source);
	if (RowAt != INDEX_NONE)
	{
		switch (Direction)
		{
		case EUINavigation::Up:
			return RowAt == 0 ? FocusChip() : FocusRow(RowAt - 1);
		case EUINavigation::Down:
			FocusRow(RowAt + 1);
			return true;
		case EUINavigation::Next:
			return ApexNav::Focus(HeaderBackButton);
		case EUINavigation::Previous:
			return FocusChip();
		case EUINavigation::Left:
			CycleLivery(-1);
			return true;
		case EUINavigation::Right:
			CycleLivery(+1);
			return true;
		default:
			return true;
		}
	}

	const int32 ChipAt = ApexNav::IndexOf(FilterChips, Source);
	if (ChipAt != INDEX_NONE)
	{
		switch (Direction)
		{
		case EUINavigation::Left:
			return ChipAt > 0 ? ApexNav::Focus(FilterChips[ChipAt - 1]) : ApexNav::Focus(HeaderBackButton);
		case EUINavigation::Right:
			if (ChipAt + 1 < FilterChips.Num())
			{
				ApexNav::Focus(FilterChips[ChipAt + 1]);
			}
			return true;
		case EUINavigation::Down:
		case EUINavigation::Next:
			FocusRow(FocusedRowIndex);
			return true;
		case EUINavigation::Previous:
			return ApexNav::Focus(HeaderBackButton);
		default:
			return true;
		}
	}

	if (Source == HeaderBackButton)
	{
		switch (Direction)
		{
		case EUINavigation::Right:
		case EUINavigation::Next:
			return FocusChip() || FocusRow(FocusedRowIndex);
		case EUINavigation::Down:
			return FocusRow(FocusedRowIndex);
		default:
			return true;
		}
	}

	return false;
}
