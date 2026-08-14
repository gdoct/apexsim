#include "UI/ApexSessionCreateWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Catalog/ApexCatalogRows.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionCreateBack(TEXT("__back"));
	const FName ActionCancel(TEXT("__cancel"));
	const FName ActionCreate(TEXT("__create"));
	const FName ActionChangeTrack(TEXT("__changetrack"));
	const FName ActionCreateChangeCar(TEXT("__changecar"));
	const FName ActionKindMultiplayer(TEXT("__kindmulti"));
	const FName ActionKindSingle(TEXT("__kindsingle"));

	/** Mode buttons carry their EApexGameMode in the action id. */
	FName ModeAction(EApexGameMode Mode)
	{
		return FName(*FString::Printf(TEXT("__mode%d"), static_cast<int32>(Mode)));
	}

	constexpr float SettingsWidth = 720.0f;
}

UApexSessionCreateWidget::UApexSessionCreateWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexSessionCreateWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexSessionCreateWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.AddDynamic(this, &UApexSessionCreateWidget::HandleLobbyStateUpdated);
	}
}

void UApexSessionCreateWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnLobbyStateUpdated.RemoveDynamic(this, &UApexSessionCreateWidget::HandleLobbyStateUpdated);
	}
	Super::NativeDestruct();
}

void UApexSessionCreateWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	RefreshContent();
	RefreshSettings();
	RefreshFooter();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexSessionCreateWidget::BuildLayout()
{
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Home");
	BackSpec.KeyCap = TEXT("Esc");
	BackSpec.bKeyCapLeading = true;
	BackSpec.Variant = EApexButtonVariant::Bare;
	BackSpec.LabelSize = 15.0f;
	BackSpec.ActionId = ActionCreateBack;

	UApexButtonWidget* BackButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	BackButton->Setup(BackSpec);
	BackButton->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);

	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Page, ApexUI::MakeScreenHeader(*WidgetTree, BackButton, TEXT("Create session")));

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Columns, BuildContentColumn(), FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(Columns, ApexUI::MakeDivider(*WidgetTree, true), FMargin(), VAlign_Fill);
	ApexUI::AddH(Columns, ApexUI::MakeSized(*WidgetTree, BuildSettingsColumn(), SettingsWidth, -1.0f), FMargin(), VAlign_Fill);

	ApexUI::AddV(Page, Columns, FMargin(), HAlign_Fill, 1.0f);
	ApexUI::AddV(Page, ApexUI::MakeDivider(*WidgetTree));
	ApexUI::AddV(Page, BuildFooter());

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Page,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
}

UWidget* UApexSessionCreateWidget::BuildContentColumn()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, TEXT("Content")), FMargin(0.0f, 0.0f, 0.0f, 14.0f));

	TrackSummaryBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(
		Column,
		ApexUI::MakePanel(*WidgetTree, TrackSummaryBox, FMargin(16.0f), ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f)),
		FMargin(0.0f, 0.0f, 0.0f, 12.0f));

	CarSummaryBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(
		Column,
		ApexUI::MakePanel(*WidgetTree, CarSummaryBox, FMargin(16.0f), ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f)));

	ApexUI::AddV(Column, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

	return ApexUI::MakePanel(
		*WidgetTree,
		Column,
		FMargin(ApexUI::Metrics::PageGutter, 26.0f, 30.0f, 26.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
}

UWidget* UApexSessionCreateWidget::BuildSettingsColumn()
{
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	// --- Session kind ---------------------------------------------------------
	ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, TEXT("Session kind")), FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	auto MakeKindButton = [this](const FString& Label, FName Action)
	{
		FApexButtonSpec Spec;
		Spec.Label = Label;
		Spec.Variant = EApexButtonVariant::Panel;
		Spec.bCentreLabel = true;
		Spec.LabelSize = 17.0f;
		Spec.Height = 48.0f;
		Spec.ActionId = Action;

		UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Button->Setup(Spec);
		Button->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);
		return Button;
	};

	KindMultiplayerButton = MakeKindButton(TEXT("Multiplayer"), ActionKindMultiplayer);
	KindSingleButton = MakeKindButton(TEXT("Single player"), ActionKindSingle);

	UHorizontalBox* KindRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(KindRow, KindMultiplayerButton, FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddH(KindRow, KindSingleButton, FMargin(8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddV(Column, KindRow, FMargin(0.0f, 0.0f, 0.0f, 26.0f));

	// --- Starting mode --------------------------------------------------------
	ApexUI::AddV(Column, ApexUI::MakeLabel(*WidgetTree, TEXT("Starting mode")), FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	struct FModeOption
	{
		EApexGameMode Mode;
		const TCHAR* Label;
		const TCHAR* Description;
		bool bAvailable;
	};

	// Replay and Qualification are left out because nothing drives them yet.
	// Demo lap is present but locked: the server drops human players from the
	// session when it starts one (game_session.rs:409-425), which leaves the
	// client a spectator with no telemetry and a countdown that never ends.
	static const FModeOption Options[] = {
		{ EApexGameMode::FreePractice, TEXT("Free practice"), TEXT("Drive freely, no limits"),        true },
		{ EApexGameMode::Sandbox,      TEXT("Sandbox"),       TEXT("Free camera, cars frozen"),       true },
		{ EApexGameMode::DemoLap,      TEXT("Demo lap"),      TEXT("Drops you to spectator"),          false },
		{ EApexGameMode::Race,         TEXT("Race"),          TEXT("Countdown, then racing"),         true },
	};

	ModeButtons.Reset();
	UVerticalBox* ModeStack = WidgetTree->ConstructWidget<UVerticalBox>();
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Options); Index += 2)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		for (int32 Column2 = 0; Column2 < 2; ++Column2)
		{
			const FModeOption& Option = Options[Index + Column2];

			FApexButtonSpec Spec;
			Spec.Label = Option.Label;
			Spec.SubLabel = Option.Description;
			Spec.Variant = Option.bAvailable ? EApexButtonVariant::Panel : EApexButtonVariant::Locked;
			Spec.Badge = Option.bAvailable ? FString() : TEXT("Locked");
			Spec.LabelSize = 18.0f;
			Spec.Height = 74.0f;
			Spec.ActionId = ModeAction(Option.Mode);

			UApexButtonWidget* Button = WidgetTree->ConstructWidget<UApexButtonWidget>();
			Button->Setup(Spec);
			Button->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);
			ModeButtons.Add(Button);

			ApexUI::AddH(Row, Button, FMargin(Column2 == 0 ? 0.0f : 8.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
		}
		ApexUI::AddV(ModeStack, Row, FMargin(0.0f, 0.0f, 0.0f, 8.0f));
	}
	ApexUI::AddV(Column, ModeStack, FMargin(0.0f, 0.0f, 0.0f, 26.0f));

	// --- Grid and length ------------------------------------------------------
	UTextBlock* PlayersValue = nullptr;
	UTextBlock* PlayersSuffix = nullptr;
	USlider* PlayersSlider = nullptr;
	UProgressBar* PlayersFill = nullptr;
	ApexUI::AddV(
		Column,
		ApexUI::MakeSliderRow(*WidgetTree, TEXT("Max players"), PlayersValue, PlayersSuffix, PlayersSlider, PlayersFill),
		FMargin(0.0f, 0.0f, 0.0f, 20.0f));
	MaxPlayersValue = PlayersValue;
	MaxPlayersSuffix = PlayersSuffix;
	MaxPlayersSlider = PlayersSlider;
	MaxPlayersFill = PlayersFill;
	MaxPlayersSlider->OnValueChanged.AddDynamic(this, &UApexSessionCreateWidget::HandleMaxPlayersChanged);

	UTextBlock* AiValue = nullptr;
	UTextBlock* AiSuffix = nullptr;
	USlider* AiSlider = nullptr;
	UProgressBar* AiFill = nullptr;
	ApexUI::AddV(
		Column,
		ApexUI::MakeSliderRow(*WidgetTree, TEXT("AI drivers"), AiValue, AiSuffix, AiSlider, AiFill));
	AiCountValue = AiValue;
	AiCountSuffix = AiSuffix;
	AiCountSlider = AiSlider;
	AiCountFill = AiFill;
	AiCountSlider->OnValueChanged.AddDynamic(this, &UApexSessionCreateWidget::HandleAiCountChanged);

	GridSummaryText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 80), ApexUI::Palette::TextMuted);
	ApexUI::AddV(Column, GridSummaryText, FMargin(0.0f, 8.0f, 0.0f, 20.0f));

	UTextBlock* LapValue = nullptr;
	UTextBlock* LapSuffix = nullptr;
	USlider* LapSlider = nullptr;
	UProgressBar* LapFill = nullptr;
	ApexUI::AddV(
		Column,
		ApexUI::MakeSliderRow(*WidgetTree, TEXT("Laps"), LapValue, LapSuffix, LapSlider, LapFill));
	LapsValue = LapValue;
	LapsSuffix = LapSuffix;
	LapsSlider = LapSlider;
	LapsFill = LapFill;
	LapsSlider->OnValueChanged.AddDynamic(this, &UApexSessionCreateWidget::HandleLapsChanged);

	ApexUI::AddV(Column, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);

	return ApexUI::MakePanel(
		*WidgetTree,
		Column,
		FMargin(34.0f, 26.0f, ApexUI::Metrics::PageGutter, 26.0f),
		ApexUI::MakeBrush(FLinearColor::Transparent));
}

UWidget* UApexSessionCreateWidget::BuildFooter()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	StatusLine = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted);
	ApexUI::AddH(Row, StatusLine);
	ApexUI::AddH(Row, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

	FApexButtonSpec CancelSpec;
	CancelSpec.Label = TEXT("Cancel");
	CancelSpec.Variant = EApexButtonVariant::Ghost;
	CancelSpec.bCentreLabel = true;
	CancelSpec.LabelSize = 17.0f;
	CancelSpec.Height = 52.0f;
	CancelSpec.ActionId = ActionCancel;

	UApexButtonWidget* CancelButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	CancelButton->Setup(CancelSpec);
	CancelButton->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, CancelButton, 170.0f, -1.0f), FMargin(0.0f, 0.0f, 12.0f, 0.0f));

	FApexButtonSpec CreateSpec;
	CreateSpec.Label = TEXT("Create session");
	CreateSpec.KeyCap = TEXT("Enter");
	CreateSpec.Variant = EApexButtonVariant::Primary;
	CreateSpec.LabelSize = 21.0f;
	CreateSpec.Height = 52.0f;
	CreateSpec.ActionId = ActionCreate;

	CreateButtonWidget = WidgetTree->ConstructWidget<UApexButtonWidget>();
	CreateButtonWidget->Setup(CreateSpec);
	CreateButtonWidget->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);
	ApexUI::AddH(Row, ApexUI::MakeSized(*WidgetTree, CreateButtonWidget, 330.0f, -1.0f));

	UBorder* Bar = ApexUI::MakePanel(
		*WidgetTree,
		Row,
		FMargin(ApexUI::Metrics::PageGutter, 0.0f),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
	Bar->SetVerticalAlignment(VAlign_Center);

	return ApexUI::MakeSized(*WidgetTree, Bar, -1.0f, 92.0f);
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void UApexSessionCreateWidget::RefreshContent()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !TrackSummaryBox || !CarSummaryBox)
	{
		return;
	}

	// --- Track ---------------------------------------------------------------
	TrackSummaryBox->ClearChildren();
	{
		FApexTrackCatalogRow Row;
		const bool bHasRow = Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row);

		FString Name = bHasRow ? Row.DisplayName : FString();
		if (Name.IsEmpty() && Net)
		{
			FApexTrackConfigSummary Summary;
			if (Net->FindTrackById(Flow->GetPendingTrackId(), Summary))
			{
				Name = Summary.Name;
			}
		}

		TArray<FString> Meta;
		if (bHasRow)
		{
			if (!Row.Country.IsEmpty())  { Meta.Add(Row.Country.ToUpper()); }
			if (Row.LengthM > 0.0f)      { Meta.Add(FString::Printf(TEXT("%.2f KM"), Row.LengthM / 1000.0f)); }
			if (!Row.Category.IsEmpty()) { Meta.Add(Row.Category.ToUpper()); }
		}

		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Title = ApexUI::MakeText(
			*WidgetTree,
			Name.IsEmpty() ? TEXT("No track selected") : Name,
			ApexUI::Font::Display(24.0f),
			Name.IsEmpty() ? ApexUI::Palette::TextDisabled : ApexUI::Palette::TextPrimary);
		Title->SetAutoWrapText(true);
		ApexUI::AddV(Text, Title);
		ApexUI::AddV(
			Text,
			ApexUI::MakeText(*WidgetTree, FString::Join(Meta, TEXT(" · ")), ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextMuted),
			FMargin(0.0f, 8.0f, 0.0f, 0.0f));

		FApexButtonSpec LinkSpec;
		LinkSpec.Label = Name.IsEmpty() ? TEXT("Select a track") : TEXT("Change track");
		LinkSpec.Variant = EApexButtonVariant::Bare;
		LinkSpec.LabelSize = 15.0f;
		LinkSpec.LabelColour = ApexUI::Palette::Accent;
		LinkSpec.ActionId = ActionChangeTrack;

		UApexButtonWidget* Link = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Link->Setup(LinkSpec);
		Link->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);
		ApexUI::AddV(Text, Link, FMargin(0.0f, 10.0f, 0.0f, 0.0f), HAlign_Left);

		UHorizontalBox* RowBox = WidgetTree->ConstructWidget<UHorizontalBox>();
		ApexUI::AddH(
			RowBox,
			ApexUI::MakePreview(*WidgetTree, bHasRow ? Row.PreviewImage.LoadSynchronous() : nullptr, TEXT("No preview"), 200.0f, 116.0f),
			FMargin(0.0f, 0.0f, 18.0f, 0.0f),
			VAlign_Center);
		ApexUI::AddH(RowBox, Text, FMargin(), VAlign_Center, 1.0f);
		ApexUI::AddV(TrackSummaryBox, RowBox);
	}

	// --- Car -----------------------------------------------------------------
	CarSummaryBox->ClearChildren();
	{
		FApexCarCatalogRow Row;
		const bool bHasRow = Flow->GetCarCatalogRow(Flow->GetPendingCarId(), Row);

		FString Name = bHasRow ? Row.DisplayName : FString();
		if (Name.IsEmpty() && Net)
		{
			FApexCarConfigSummary Summary;
			if (Net->FindCarById(Flow->GetPendingCarId(), Summary))
			{
				Name = Summary.Name;
			}
		}

		TArray<FString> Meta;
		if (bHasRow)
		{
			if (!Row.CarClass.IsEmpty()) { Meta.Add(Row.CarClass.ToUpper()); }
			if (Row.MaxPowerKw > 0.0f)   { Meta.Add(FString::Printf(TEXT("%.0f HP"), Row.MaxPowerKw * 1.34102f)); }
			if (Row.MassKg > 0.0f)       { Meta.Add(FString::Printf(TEXT("%.0f KG"), Row.MassKg)); }
		}

		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Text, ApexUI::MakeText(
			*WidgetTree,
			Name.IsEmpty() ? TEXT("No car selected") : Name,
			ApexUI::Font::Display(24.0f),
			Name.IsEmpty() ? ApexUI::Palette::TextDisabled : ApexUI::Palette::TextPrimary));
		ApexUI::AddV(
			Text,
			ApexUI::MakeText(*WidgetTree, FString::Join(Meta, TEXT(" · ")), ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextMuted),
			FMargin(0.0f, 8.0f, 0.0f, 0.0f));

		FApexButtonSpec LinkSpec;
		LinkSpec.Label = Name.IsEmpty() ? TEXT("Select a car") : TEXT("Change car");
		LinkSpec.Variant = EApexButtonVariant::Bare;
		LinkSpec.LabelSize = 15.0f;
		LinkSpec.LabelColour = ApexUI::Palette::Accent;
		LinkSpec.ActionId = ActionCreateChangeCar;

		UApexButtonWidget* Link = WidgetTree->ConstructWidget<UApexButtonWidget>();
		Link->Setup(LinkSpec);
		Link->OnActivated.AddDynamic(this, &UApexSessionCreateWidget::HandleButtonActivated);
		ApexUI::AddV(Text, Link, FMargin(0.0f, 10.0f, 0.0f, 0.0f), HAlign_Left);

		UHorizontalBox* RowBox = WidgetTree->ConstructWidget<UHorizontalBox>();
		// Cars have meshes rather than preview textures, and the turntable belongs
		// to the garage; a captioned placeholder is honest here.
		ApexUI::AddH(
			RowBox,
			ApexUI::MakeSized(*WidgetTree, ApexUI::MakeArtPlaceholder(*WidgetTree, TEXT("Car")), 200.0f, 116.0f),
			FMargin(0.0f, 0.0f, 18.0f, 0.0f),
			VAlign_Center);
		ApexUI::AddH(RowBox, Text, FMargin(), VAlign_Center, 1.0f);
		ApexUI::AddV(CarSummaryBox, RowBox);
	}
}

void UApexSessionCreateWidget::RefreshSettings()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	if (KindMultiplayerButton && KindSingleButton)
	{
		const bool bSingle = Flow->CreateSessionKind == EApexSessionKind::Practice;
		KindMultiplayerButton->SetSelected(!bSingle);
		KindSingleButton->SetSelected(bSingle);
	}

	for (UApexButtonWidget* Button : ModeButtons)
	{
		if (Button)
		{
			Button->SetSelected(Button->GetActionId() == ModeAction(Flow->CreateStartingMode));
		}
	}

	// Sliders are normalised 0..1; the labels carry the real numbers.
	if (MaxPlayersSlider)
	{
		MaxPlayersSlider->SetValue(static_cast<float>(Flow->CreateMaxPlayers - 1) / (MaxPlayersCeiling - 1));
	}
	if (AiCountSlider)
	{
		AiCountSlider->SetValue(static_cast<float>(Flow->CreateAiCount) / FMath::Max(1, MaxPlayersCeiling - 1));
	}
	if (LapsSlider)
	{
		LapsSlider->SetValue(static_cast<float>(Flow->CreateLapLimit - 1) / (LapsCeiling - 1));
	}

	if (MaxPlayersValue)  { MaxPlayersValue->SetText(FText::AsNumber(Flow->CreateMaxPlayers)); }
	if (MaxPlayersSuffix) { MaxPlayersSuffix->SetText(FText::FromString(FString::Printf(TEXT("/ %d"), MaxPlayersCeiling))); }
	if (MaxPlayersFill)   { MaxPlayersFill->SetPercent(static_cast<float>(Flow->CreateMaxPlayers) / MaxPlayersCeiling); }

	if (AiCountValue)  { AiCountValue->SetText(FText::AsNumber(Flow->CreateAiCount)); }
	if (AiCountSuffix) { AiCountSuffix->SetText(FText::FromString(FString::Printf(TEXT("/ %d"), MaxPlayersCeiling - 1))); }
	if (AiCountFill)   { AiCountFill->SetPercent(static_cast<float>(Flow->CreateAiCount) / (MaxPlayersCeiling - 1)); }

	if (LapsValue)  { LapsValue->SetText(FText::AsNumber(Flow->CreateLapLimit)); }
	if (LapsFill)   { LapsFill->SetPercent(static_cast<float>(Flow->CreateLapLimit) / LapsCeiling); }

	if (LapsSuffix)
	{
		// The only honest estimate available: laps times a lap this player has
		// actually driven here. No personal best, no estimate.
		float BestSeconds = 0.0f;
		if (Flow->GetBestLapSeconds(Flow->GetPendingTrackId(), BestSeconds) && BestSeconds > 0.0f)
		{
			const int32 TotalSeconds = FMath::RoundToInt(BestSeconds * Flow->CreateLapLimit);
			LapsSuffix->SetText(FText::FromString(
				FString::Printf(TEXT("≈ %d:%02d at your best"), TotalSeconds / 60, TotalSeconds % 60)));
		}
		else
		{
			LapsSuffix->SetText(FText::GetEmpty());
		}
	}

	if (GridSummaryText)
	{
		const int32 Slots = Flow->CreateMaxPlayers;
		const int32 Ai = FMath::Min(Flow->CreateAiCount, FMath::Max(0, Slots - 1));
		const int32 Open = FMath::Max(0, Slots - 1 - Ai);
		GridSummaryText->SetText(FText::FromString(FString::Printf(
			TEXT("GRID: %d SLOTS · 1 HUMAN + %d AI · %d OPEN"), Slots, Ai, Open)));
	}
}

void UApexSessionCreateWidget::RefreshFooter()
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexNetSubsystem* Net = GetNet();
	if (!Flow || !StatusLine)
	{
		return;
	}

	const bool bConnected = Net && Net->IsAuthenticated();
	const bool bHasTrack = Flow->HasPendingTrack();
	const bool bHasCar = Flow->HasPendingCar();

	FString Status;
	FLinearColor Colour = ApexUI::Palette::TextMuted;

	if (!bConnected)
	{
		Status = TEXT("NOT CONNECTED — CONNECT TO A SERVER FIRST");
		Colour = ApexUI::Palette::Error;
	}
	else if (!bHasTrack)
	{
		Status = TEXT("PICK A TRACK TO CONTINUE");
		Colour = ApexUI::Palette::Error;
	}
	else if (!bHasCar)
	{
		Status = TEXT("READY · NO CAR PICKED, THE SERVER WILL ASK FOR ONE");
		Colour = ApexUI::Palette::Accent;
	}
	else
	{
		Status = TEXT("READY · TRACK AND CAR SELECTED");
		Colour = ApexUI::Palette::Live;
	}

	StatusLine->SetText(FText::FromString(Status));
	StatusLine->SetColorAndOpacity(FSlateColor(Colour));

	if (CreateButtonWidget)
	{
		CreateButtonWidget->SetIsEnabled(bConnected && bHasTrack);
	}
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void UApexSessionCreateWidget::HandleLobbyStateUpdated(const FApexLobbyState& LobbyState)
{
	RefreshContent();
	RefreshFooter();
}

void UApexSessionCreateWidget::HandleMaxPlayersChanged(float Value)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	Flow->CreateMaxPlayers = FMath::Clamp(FMath::RoundToInt(Value * (MaxPlayersCeiling - 1)) + 1, 1, MaxPlayersCeiling);
	// AI can never outnumber the grid minus the player.
	Flow->CreateAiCount = FMath::Min(Flow->CreateAiCount, FMath::Max(0, Flow->CreateMaxPlayers - 1));
	Flow->SaveProfile();
	RefreshSettings();
}

void UApexSessionCreateWidget::HandleAiCountChanged(float Value)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	const int32 Ceiling = FMath::Max(0, FMath::Min(MaxPlayersCeiling - 1, Flow->CreateMaxPlayers - 1));
	Flow->CreateAiCount = FMath::Clamp(FMath::RoundToInt(Value * (MaxPlayersCeiling - 1)), 0, Ceiling);
	Flow->SaveProfile();
	RefreshSettings();
}

void UApexSessionCreateWidget::HandleLapsChanged(float Value)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Flow)
	{
		return;
	}

	Flow->CreateLapLimit = FMath::Clamp(FMath::RoundToInt(Value * (LapsCeiling - 1)) + 1, 1, LapsCeiling);
	Flow->SaveProfile();
	RefreshSettings();
}

void UApexSessionCreateWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	if (!Button || !Flow)
	{
		return;
	}

	const FName Id = Button->GetActionId();

	if (Id == ActionCreateBack || Id == ActionCancel)
	{
		GoBack();
		return;
	}

	if (Id == ActionChangeTrack)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterTrackSelect = EApexScreen::SessionCreate;
		}
		ShowScreen(EApexScreen::TrackSelect);
		return;
	}

	if (Id == ActionCreateChangeCar)
	{
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ScreenAfterCarSelect = EApexScreen::SessionCreate;
		}
		ShowScreen(EApexScreen::CarSelect);
		return;
	}

	if (Id == ActionKindMultiplayer || Id == ActionKindSingle)
	{
		// "Single player" is the protocol's Practice kind: the same session, not
		// listed for others to join.
		Flow->CreateSessionKind = Id == ActionKindSingle ? EApexSessionKind::Practice : EApexSessionKind::Multiplayer;
		Flow->SaveProfile();
		RefreshSettings();
		return;
	}

	if (Id == ActionCreate)
	{
		UApexNetSubsystem* Net = GetNet();
		if (!Net || !Net->IsAuthenticated())
		{
			ShowToast(TEXT("Not connected to a server"), true);
			return;
		}
		if (!Flow->HasPendingTrack())
		{
			ShowToast(TEXT("Pick a track first"), true);
			return;
		}

		if (Flow->HasPendingCar())
		{
			Net->SelectCar(Flow->GetPendingCarId());
		}

		// Created sessions land in the lobby: other people may still be joining,
		// and it is the host who decides when to count in.
		Flow->bAutoStartOnJoin = false;
		Flow->SaveProfile();

		UE_LOG(LogApexSim, Log, TEXT("Create session: track '%s', %d players, %d AI, %d laps, kind %d"),
			*Flow->GetPendingTrackId(), Flow->CreateMaxPlayers, Flow->CreateAiCount,
			Flow->CreateLapLimit, static_cast<int32>(Flow->CreateSessionKind));

		Net->CreateSession(
			Flow->GetPendingTrackId(),
			Flow->CreateMaxPlayers,
			Flow->CreateAiCount,
			Flow->CreateLapLimit,
			Flow->CreateSessionKind);
		return;
	}

	// Otherwise a starting-mode tile.
	for (int32 Mode = 0; Mode <= static_cast<int32>(EApexGameMode::Race); ++Mode)
	{
		if (Id == ModeAction(static_cast<EApexGameMode>(Mode)))
		{
			Flow->CreateStartingMode = static_cast<EApexGameMode>(Mode);
			Flow->SaveProfile();
			RefreshSettings();
			return;
		}
	}
}
