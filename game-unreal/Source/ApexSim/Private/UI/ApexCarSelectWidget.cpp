#include "UI/ApexCarSelectWidget.h"

#include "ApexCarPreviewStage.h"
#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
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
#include "Engine/TextureRenderTarget2D.h"
#include "TimerManager.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionCarSelectBack(TEXT("__back"));
	const FName ActionDrive(TEXT("__drive"));
	const FName ActionSetup(TEXT("__setup"));

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
	ApexUI::AddV(Column, StageFrame, FMargin(), HAlign_Fill, 1.0f);

	// Name and actions.
	UVerticalBox* NameStack = WidgetTree->ConstructWidget<UVerticalBox>();
	EyebrowText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 160), ApexUI::Palette::Accent);
	ApexUI::AddV(NameStack, EyebrowText);
	NameText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(34.0f), ApexUI::Palette::TextPrimary);
	ApexUI::AddV(NameStack, NameText, FMargin(0.0f, 8.0f, 0.0f, 0.0f));

	UHorizontalBox* ActionRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(ActionRow, NameStack, FMargin(), VAlign_Center, 1.0f);

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

	FApexButtonSpec DriveSpec;
	DriveSpec.Label = TEXT("Drive this car");
	DriveSpec.KeyCap = TEXT("Enter");
	DriveSpec.Variant = EApexButtonVariant::Primary;
	DriveSpec.LabelSize = 21.0f;
	DriveSpec.Height = 56.0f;
	DriveSpec.ActionId = ActionDrive;

	DriveButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	DriveButton->Setup(DriveSpec);
	DriveButton->OnActivated.AddDynamic(this, &UApexCarSelectWidget::HandleButtonActivated);
	ApexUI::AddH(ActionRow, ApexUI::MakeSized(*WidgetTree, DriveButton, 300.0f, -1.0f), FMargin(12.0f, 0.0f, 0.0f, 0.0f));

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
		Spec.Badge = bHasRow ? Row.CarClass : FString();
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
				Classes.AddUnique(Row.CarClass);
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
				&& Row.CarClass.Equals(ActiveFilter, ESearchCase::IgnoreCase);
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
			if (!Row.CarClass.IsEmpty())            { Parts.Add(Row.CarClass.ToUpper()); }
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
		ClassValue->SetText(FText::FromString(bHasRow && !Row.CarClass.IsEmpty() ? Row.CarClass : TEXT("—")));
	}

	if (PowerBar)  { PowerBar->SetPercent(MaxHp > 0.0f ? Hp / MaxHp : 0.0f); }
	if (WeightBar) { WeightBar->SetPercent(MaxMass > 0.0f ? Mass / MaxMass : 0.0f); }
	if (RatioBar)  { RatioBar->SetPercent(MaxRatio > 0.0f ? Ratio / MaxRatio : 0.0f); }
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
		Stage->SetCarMesh(nullptr);
		return;
	}

	Stage->SetPreviewTransform(Row.PreviewOffset, Row.PreviewRotation, Row.PreviewScale);
	Stage->SetCarMesh(Row.Mesh);
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
	if (Row)
	{
		SelectCar(Row->GetActionId().ToString());
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

	if (Id == ActionDrive)
	{
		if (SelectedCarId.IsEmpty())
		{
			ShowToast(TEXT("Pick a car first"), true);
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
			Net->SelectCar(SelectedCarId);
		}
		if (UApexRootWidget* Root = GetRoot())
		{
			Root->ReplaceScreen(Root->ScreenAfterCarSelect);
		}
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
		case EUINavigation::Right:
		case EUINavigation::Next:
			return ApexNav::Focus(DriveButton);
		case EUINavigation::Previous:
			return FocusChip();
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
			return FocusRow(FocusedRowIndex) || ApexNav::Focus(DriveButton);
		case EUINavigation::Previous:
			return ApexNav::Focus(HeaderBackButton);
		default:
			return true;
		}
	}

	if (Source == DriveButton)
	{
		switch (Direction)
		{
		case EUINavigation::Left:
		case EUINavigation::Previous:
			return FocusRow(FocusedRowIndex);
		case EUINavigation::Up:
		case EUINavigation::Next:
			return FocusChip();
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
