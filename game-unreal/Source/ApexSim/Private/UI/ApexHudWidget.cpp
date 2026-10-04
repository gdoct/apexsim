#include "UI/ApexHudWidget.h"

#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "Hud/ApexHudDataSubsystem.h"
#include "ImageUtils.h"
#include "Race/ApexRaceDirector.h"
#include "UI/ApexMinimapWidget.h"
#include "UI/ApexMirrorWidget.h"
#include "UI/ApexUIStyle.h"
#include "UObject/UObjectIterator.h"

// The host is nothing but style primitives; qualifying every one of them would
// double the length of each layout line without adding any information.
using namespace ApexUI;

namespace
{
	/** Clear of the screen's edges, as the menu screens are. */
	constexpr float HudEdgeGutter = 30.0f;

	/** The virtual mirror's glass when a component does not size it; the same 3.2:1 as the capture. */
	const FVector2D HudDefaultMirrorSize(480.0f, 150.0f);

	FAutoConsoleCommand HudReloadCommand(TEXT("apexsim.hud.Reload"),
		TEXT("Read the HUD components (content/hud, -ApexHudDir) again and rebuild the HUD."),
		FConsoleCommandDelegate::CreateLambda([]() {
			int32 Count = 0;
			for (TObjectIterator<UApexHudWidget> It; It; ++It)
			{
				if (!It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && It->GetWorld())
				{
					It->Reload();
					++Count;
				}
			}
			UE_LOG(LogApexSim, Display, TEXT("apexsim.hud.Reload: %d HUD(s) rebuilt"), Count);
		}));

	struct FHudRegionLayout
	{
		EHorizontalAlignment H = HAlign_Left;
		EVerticalAlignment V = VAlign_Top;
		/** Components in the region sit side by side (the top and bottom bands) or stacked (the sides and middle). */
		bool bHorizontal = true;
	};

	FHudRegionLayout HudRegionLayout(const FString& Region)
	{
		FHudRegionLayout Out;
		Out.H = Region.EndsWith(TEXT("left")) ? HAlign_Left : Region.EndsWith(TEXT("right")) ? HAlign_Right : HAlign_Center;
		Out.V = Region.StartsWith(TEXT("top")) ? VAlign_Top : Region.StartsWith(TEXT("bottom")) ? VAlign_Bottom : VAlign_Center;
		Out.bHorizontal = Out.V != VAlign_Center;
		return Out;
	}

	FSlateFontInfo HudFont(const FApexHudElementDef& Def, bool bBold)
	{
		if (Def.FontFace == TEXT("display"))
		{
			return Font::Display(Def.FontSize, Def.Tracking);
		}
		if (Def.FontFace == TEXT("mono"))
		{
			return Font::Mono(Def.FontSize, Def.Tracking);
		}
		FSlateFontInfo Body = Font::Body(Def.FontSize, bBold);
		Body.LetterSpacing = Def.Tracking;
		return Body;
	}

	EProgressBarFillType::Type HudBarFill(const FString& Direction)
	{
		return Direction == TEXT("left") ? EProgressBarFillType::RightToLeft
			: Direction == TEXT("up") ? EProgressBarFillType::BottomToTop
			: Direction == TEXT("down") ? EProgressBarFillType::TopToBottom
			: EProgressBarFillType::LeftToRight;
	}

	/** Re-evaluate a colour attribute; true when it changed (or was never applied). */
	bool HudUpdateColour(const FApexHudProp& Prop, const FApexHudScope& Scope, bool& bHas, FLinearColor& Last)
	{
		if (!Prop.IsSet() || (bHas && !Prop.IsDynamic()))
		{
			return false;
		}
		FLinearColor Colour;
		if (!Prop.Expr->Evaluate(Scope).AsColour(Colour))
		{
			// Not a colour this frame (null, a misspelt name): keep what is there.
			return false;
		}
		if (bHas && Colour == Last)
		{
			return false;
		}
		bHas = true;
		Last = Colour;
		return true;
	}
}

UApexHudWidget::UApexHudWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Collapsed until a race starts.
	//
	// Not HitTestInvisible-by-default: the shell only calls SetRaceActive when
	// the race view *changes*, so a HUD that starts visible is never told to go
	// away and sits on top of every menu screen from launch. HitTestInvisible is
	// what it switches to once racing: clicks have to reach the race view.
	SetVisibility(ESlateVisibility::Collapsed);
}

UApexHudDataSubsystem* UApexHudWidget::GetHudData() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexHudDataSubsystem>() : nullptr;
}

// --- Lifecycle --------------------------------------------------------------

void UApexHudWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	Reload();
}

void UApexHudWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (UApexSettingsSubsystem* Settings = GameInstance->GetSubsystem<UApexSettingsSubsystem>())
		{
			Settings->OnSettingsChanged.AddDynamic(this, &UApexHudWidget::HandleSettingsChanged);
		}
	}
}

void UApexHudWidget::NativeDestruct()
{
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (UApexSettingsSubsystem* Settings = GameInstance->GetSubsystem<UApexSettingsSubsystem>())
		{
			Settings->OnSettingsChanged.RemoveDynamic(this, &UApexHudWidget::HandleSettingsChanged);
		}
	}
	Super::NativeDestruct();
}

void UApexHudWidget::Reload()
{
	Components.Reset();
	Report = FApexHudLoadReport();
	const TArray<FString> Directories = ApexHud::HudDirectories();
	ApexHud::LoadComponents(Directories, Components, Report);

	for (const FString& Error : Report.Errors)
	{
		UE_LOG(LogApexSim, Warning, TEXT("HUD component left out: %s"), *Error);
	}
	for (const FString& Warning : Report.Warnings)
	{
		UE_LOG(LogApexSim, Warning, TEXT("HUD component: %s"), *Warning);
	}
	UE_LOG(LogApexSim, Log, TEXT("HUD: %d component(s) from %s"), Components.Num(), *FString::Join(Directories, TEXT(" + ")));

	// The player's arrangement, from the HUD editor. A broken file is a
	// warning and the shipped layout, never a missing HUD.
	LayoutFile = FApexHudLayout::DefaultFile();
	FString LayoutError;
	if (!Layout.Load(LayoutFile, LayoutError))
	{
		UE_LOG(LogApexSim, Warning, TEXT("HUD layout %s ignored: %s"), *LayoutFile, *LayoutError);
		Layout = FApexHudLayout();
	}
	else if (!LayoutError.IsEmpty())
	{
		UE_LOG(LogApexSim, Warning, TEXT("HUD layout %s: %s"), *LayoutFile, *LayoutError);
	}
	if (Components.IsEmpty())
	{
		UE_LOG(LogApexSim, Warning, TEXT("HUD: no components found; the race will have no HUD"));
	}

	BuildHud();
	ApplyVisibility();
}

void UApexHudWidget::SetRaceActive(bool bActive)
{
	if (bActive && !bRaceActive)
	{
		// The outline is only fetched while the map is empty, and the HUD lives
		// on from one race to the next: without this, a second race on another
		// circuit kept drawing the first one's shape under the blips.
		for (UApexMinimapWidget* Minimap : Minimaps)
		{
			if (Minimap)
			{
				Minimap->SetCenterline(TArray<FVector2D>());
			}
		}
		bMinimapOutlineSet = false;
	}
	bRaceActive = bActive;
	if (UApexHudDataSubsystem* Data = GetHudData())
	{
		Data->SetRaceActive(bActive);
	}
	ApplyVisibility();
}

void UApexHudWidget::SetShown(bool bShown)
{
	if (bShownWanted != bShown)
	{
		bShownWanted = bShown;
		ApplyVisibility();
	}
}

void UApexHudWidget::HandleSettingsChanged(EApexSettingsGroup Group)
{
	// The detail level hides the whole HUD or, through `hud.full`, the
	// optional components; the components follow it on the next frame.
	if (Group == EApexSettingsGroup::Gameplay)
	{
		ApplyVisibility();
	}
}

void UApexHudWidget::ApplyVisibility()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	const EApexHudDetail Detail = Settings && Settings->Get() ? Settings->Get()->HudDetail : EApexHudDetail::All;
	// The editor shows the HUD whatever the detail level, race or no race:
	// the panels being laid out have to be on screen.
	SetVisibility(bEditing || (bRaceActive && bShownWanted && Detail != EApexHudDetail::Hidden)
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);
	if (EditBackdrop)
	{
		EditBackdrop->SetVisibility(bEditing && !bRaceActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UApexHudWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	UApexHudDataSubsystem* Hud = GetHudData();
	if ((!bRaceActive && !bEditing) || GetVisibility() == ESlateVisibility::Collapsed || !Hud)
	{
		return;
	}

	Hud->Refresh();
	FApexHudScope Scope;
	Scope.Data = &Hud->GetData();

	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		const int32 Root = ComponentRoots.IsValidIndex(Index) ? ComponentRoots[Index] : INDEX_NONE;
		if (Root == INDEX_NONE)
		{
			continue;
		}
		const FApexHudComponentDef& Component = Components[Index];
		// While laying out, every shown panel is on screen: a damage panel that
		// only appears after a hit could not be placed otherwise.
		if (!bEditing && Component.Visible.IsSet() && !Component.Visible.Expr->Evaluate(Scope).AsBool())
		{
			SetNodeVisible(Nodes[Root], false);
			continue;
		}
		UpdateNode(Root, Scope);
	}
}

// --- Construction -------------------------------------------------------------

void UApexHudWidget::BuildHud()
{
	// Every node points into the old tree, which is about to go.
	Nodes.Reset();
	ComponentRoots.Reset();
	ComponentWrappers.Reset();
	ComponentCanvasSlots.Reset();
	Minimaps.Reset();
	Mirrors.Reset();
	Images.Reset();
	ErrorText = nullptr;
	bMinimapOutlineSet = false;

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();
	const FMargin Gutter(Metrics::PageGutter, HudEdgeGutter);

	{
		FLinearColor Backdrop = Palette::Background;
		Backdrop.A = 0.94f;
		EditBackdrop = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(Backdrop));
		UOverlaySlot* BackdropSlot = Layers->AddChildToOverlay(EditBackdrop);
		BackdropSlot->SetHorizontalAlignment(HAlign_Fill);
		BackdropSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// Components sit in their region in order; equal orders by name, so the
	// layout is the same on every machine.
	TArray<int32> Placement;
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		Placement.Add(Index);
	}
	Placement.Sort([this](int32 A, int32 B)
	{
		const FApexHudComponentDef& X = Components[A];
		const FApexHudComponentDef& Y = Components[B];
		return X.Order != Y.Order ? X.Order < Y.Order : X.Id < Y.Id;
	});

	ComponentRoots.Init(INDEX_NONE, Components.Num());
	ComponentWrappers.Init(nullptr, Components.Num());
	ComponentCanvasSlots.Init(nullptr, Components.Num());
	TMap<FString, UPanelWidget*> RegionBoxes;

	// Pinned components sit on a canvas over the regions, each with its anchor
	// corner at its anchor point of the screen.
	PinCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();

	for (const int32 Index : Placement)
	{
		const FApexHudComponentDef& Component = Components[Index];
		if (!Layout.IsEnabled(Component.Id, Component.bDefaultEnabled))
		{
			continue;
		}
		const int32 Root = BuildElement(Component.Root, INDEX_NONE);
		ComponentRoots[Index] = Root;

		// Every component is scaled by the layout, 100% unless resized.
		UScaleBox* Wrapper = WidgetTree->ConstructWidget<UScaleBox>();
		Wrapper->SetStretch(EStretch::UserSpecified);
		Wrapper->SetUserSpecifiedScale(Layout.ScaleOf(Component.Id));
		Wrapper->AddChild(Nodes[Root].Outer);
		ComponentWrappers[Index] = Wrapper;
		UWidget* Widget = Wrapper;

		const FApexHudPlacement* Pinned = Layout.Find(Component.Id);
		if (Pinned && Pinned->bPinned)
		{
			UCanvasPanelSlot* PinSlot = PinCanvas->AddChildToCanvas(Wrapper);
			PinSlot->SetAutoSize(true);
			PinSlot->SetAnchors(FAnchors(Pinned->Anchor.X, Pinned->Anchor.Y));
			PinSlot->SetAlignment(Pinned->Anchor);
			PinSlot->SetPosition(Pinned->Position);
			ComponentCanvasSlots[Index] = PinSlot;
			continue;
		}

		const FHudRegionLayout Region = HudRegionLayout(Component.Region);

		if (Component.bFloat)
		{
			// On its own: comes and goes without moving the region's row.
			UOverlaySlot* HudSlot = Layers->AddChildToOverlay(Widget);
			HudSlot->SetHorizontalAlignment(Region.H);
			HudSlot->SetVerticalAlignment(Region.V);
			HudSlot->SetPadding(Gutter + Component.Margin);
			continue;
		}

		UPanelWidget*& Box = RegionBoxes.FindOrAdd(Component.Region);
		if (!Box)
		{
			Box = Region.bHorizontal
				? static_cast<UPanelWidget*>(WidgetTree->ConstructWidget<UHorizontalBox>())
				: static_cast<UPanelWidget*>(WidgetTree->ConstructWidget<UVerticalBox>());
			UOverlaySlot* HudSlot = Layers->AddChildToOverlay(Box);
			HudSlot->SetHorizontalAlignment(Region.H);
			HudSlot->SetVerticalAlignment(Region.V);
			HudSlot->SetPadding(Gutter);
		}
		if (UHorizontalBox* Row = Cast<UHorizontalBox>(Box))
		{
			// The band's components line up along the screen's edge.
			AddH(Row, Widget, Component.Margin, Region.V == VAlign_Top ? VAlign_Top : VAlign_Bottom);
		}
		else if (UVerticalBox* Column = Cast<UVerticalBox>(Box))
		{
			AddV(Column, Widget, Component.Margin, Region.H);
		}
	}

	UOverlaySlot* CanvasSlot = Layers->AddChildToOverlay(PinCanvas);
	CanvasSlot->SetHorizontalAlignment(HAlign_Fill);
	CanvasSlot->SetVerticalAlignment(VAlign_Fill);

	// A component that could not be loaded says so on screen: a missing panel
	// with the reason only in the log reads as a game bug.
	if (!Report.Errors.IsEmpty())
	{
		TArray<FString> Lines = Report.Errors;
		if (Lines.Num() > 6)
		{
			Lines.SetNum(6);
			Lines.Add(TEXT("…"));
		}
		const FString Message = FString::Printf(TEXT("HUD: %d component(s) left out (apexsim.hud.Reload after a fix)\n%s"),
			Report.Errors.Num(), *FString::Join(Lines, TEXT("\n")));
		ErrorText = MakeText(*WidgetTree, Message, Font::Mono(11.0f), Palette::Error);
		ErrorText->SetAutoWrapText(true);
		UWidget* Panel = MakeSized(*WidgetTree, MakePanel(*WidgetTree, ErrorText, FMargin(12.0f, 8.0f), MakeBrush(Palette::Background)), 720.0f, -1.0f);
		UOverlaySlot* HudSlot = Layers->AddChildToOverlay(Panel);
		HudSlot->SetHorizontalAlignment(HAlign_Center);
		HudSlot->SetVerticalAlignment(VAlign_Center);
	}

	// The root is set once and the tree is swapped inside it: a user widget
	// builds its Slate widgets from RootWidget the first time it is shown and
	// never looks again, so a rebuild that replaced the root (apexsim.hud.Reload,
	// every change in the HUD editor) left the old tree frozen on screen.
	if (!HostRoot)
	{
		HostRoot = WidgetTree->ConstructWidget<UOverlay>();
		WidgetTree->RootWidget = HostRoot;
	}
	HostRoot->ClearChildren();
	UOverlaySlot* LayersSlot = HostRoot->AddChildToOverlay(Layers);
	LayersSlot->SetHorizontalAlignment(HAlign_Fill);
	LayersSlot->SetVerticalAlignment(VAlign_Fill);
}

int32 UApexHudWidget::BuildElement(const FApexHudElementDef& Def, int32 Copy)
{
	const int32 Index = Nodes.AddDefaulted();
	Nodes[Index].Def = &Def;
	Nodes[Index].Slot = Copy;

	UWidget* Widget = nullptr;
	UPanelWidget* Container = nullptr;
	UBorder* SingleChildPanel = nullptr;

	switch (Def.Type)
	{
	case EApexHudElementType::Row:
		Container = WidgetTree->ConstructWidget<UHorizontalBox>();
		Widget = Container;
		break;
	case EApexHudElementType::Column:
		Container = WidgetTree->ConstructWidget<UVerticalBox>();
		Widget = Container;
		break;
	case EApexHudElementType::Stack:
		Container = WidgetTree->ConstructWidget<UOverlay>();
		Widget = Container;
		break;
	case EApexHudElementType::Panel:
	{
		UBorder* Border = MakePanel(*WidgetTree, nullptr, Def.Padding, MakeBrush(FLinearColor::Transparent));
		Widget = Border;
		// One plain child sits straight in the panel, aligned by its own
		// halign/valign; anything more goes in a box laid out by `direction`.
		if (Def.Children.Num() == 1 && !Def.Children[0].Repeat.IsSet())
		{
			SingleChildPanel = Border;
		}
		else
		{
			Container = Def.Direction == EApexHudElementType::Row ? static_cast<UPanelWidget*>(WidgetTree->ConstructWidget<UHorizontalBox>())
				: Def.Direction == EApexHudElementType::Stack ? static_cast<UPanelWidget*>(WidgetTree->ConstructWidget<UOverlay>())
				: static_cast<UPanelWidget*>(WidgetTree->ConstructWidget<UVerticalBox>());
			Border->SetContent(Container);
		}
		break;
	}
	case EApexHudElementType::Text:
	case EApexHudElementType::Label:
	{
		UTextBlock* Text = MakeText(*WidgetTree, FString(), HudFont(Def, false), Palette::TextPrimary);
		Text->SetJustification(Def.Justify);
		Widget = Text;
		break;
	}
	case EApexHudElementType::Rect:
		Widget = MakePanel(*WidgetTree, nullptr, FMargin(), MakeBrush(FLinearColor::Transparent));
		break;
	case EApexHudElementType::Bar:
	{
		UProgressBar* Bar = WidgetTree->ConstructWidget<UProgressBar>();
		FProgressBarStyle Style = Bar->GetWidgetStyle();
		Style.SetBackgroundImage(MakeBrush(Palette::Surface));
		Style.SetFillImage(MakeBrush(Palette::Accent));
		Bar->SetWidgetStyle(Style);
		Bar->SetBarFillType(HudBarFill(Def.BarDirection));
		Bar->SetPercent(0.0f);
		Widget = Bar;
		break;
	}
	case EApexHudElementType::Spacer:
		Widget = WidgetTree->ConstructWidget<USpacer>();
		break;
	case EApexHudElementType::Divider:
		Widget = MakeDivider(*WidgetTree, Def.bVertical);
		break;
	case EApexHudElementType::KeyCap:
		Widget = MakeKeyCap(*WidgetTree, Def.Text.IsSet() ? Def.Text.Expr->Evaluate(FApexHudScope()).AsString() : FString());
		break;
	case EApexHudElementType::Image:
	{
		UImage* Image = WidgetTree->ConstructWidget<UImage>();
		if (UTexture2D* Texture = FImageUtils::ImportFileAsTexture2D(Def.ImageFile))
		{
			Images.Add(Texture);
			Image->SetBrushFromTexture(Texture, Def.Width < 0.0f && Def.Height < 0.0f);
		}
		Widget = Image;
		break;
	}
	case EApexHudElementType::Minimap:
	{
		UApexMinimapWidget* Minimap = WidgetTree->ConstructWidget<UApexMinimapWidget>();
		Minimaps.Add(Minimap);
		Widget = Minimap;
		break;
	}
	case EApexHudElementType::Mirror:
	{
		UApexMirrorWidget* Mirror = WidgetTree->ConstructWidget<UApexMirrorWidget>();
		Mirror->SetFaceSize(Def.Width > 0.0f && Def.Height > 0.0f ? FVector2D(Def.Width, Def.Height) : HudDefaultMirrorSize);
		Mirrors.Add(Mirror);
		Widget = Mirror;
		break;
	}
	}

	UWidget* Outer = Widget;
	// The mirror sizes its own glass; everything else gets a size box, which
	// is also what is hidden, so a hidden element leaves no gap.
	if ((Def.Width >= 0.0f || Def.Height >= 0.0f) && Def.Type != EApexHudElementType::Mirror)
	{
		Outer = MakeSized(*WidgetTree, Widget, Def.Width, Def.Height);
	}
	Nodes[Index].Widget = Widget;
	Nodes[Index].Outer = Outer;

	for (const FApexHudElementDef& ChildDef : Def.Children)
	{
		const int32 Copies = ChildDef.Repeat.IsSet() ? ChildDef.Repeat.Slots() : 1;
		for (int32 Instance = 0; Instance < Copies; ++Instance)
		{
			// Nodes may reallocate under the recursion: hold indices, not references.
			const int32 Child = BuildElement(ChildDef, ChildDef.Repeat.IsSet() ? Instance : INDEX_NONE);
			Nodes[Index].Children.Add(Child);
			if (SingleChildPanel)
			{
				// Alignment and padding first: the border's slot copies them when
				// the content goes in, and it is the slot's that the Slate border
				// is built with. Set afterwards, before the widget exists, they
				// were dropped and every child filled its panel from the top left
				// (the compound letter sat up and left in its ring).
				SingleChildPanel->SetHorizontalAlignment(ChildDef.HAlign.Get(HAlign_Fill));
				SingleChildPanel->SetVerticalAlignment(ChildDef.VAlign.Get(VAlign_Fill));
				SingleChildPanel->SetPadding(Def.Padding + ChildDef.Margin);
				SingleChildPanel->SetContent(Nodes[Child].Outer);
			}
			else if (Container)
			{
				AddToContainer(Container, Nodes[Child]);
			}
		}
	}
	return Index;
}

void UApexHudWidget::AddToContainer(UPanelWidget* Container, const FHudNode& Child)
{
	const FApexHudElementDef& Def = *Child.Def;
	if (UHorizontalBox* Row = Cast<UHorizontalBox>(Container))
	{
		UHorizontalBoxSlot* HudSlot = AddH(Row, Child.Outer, Def.Margin, Def.VAlign.Get(VAlign_Center), Def.Fill);
		if (HudSlot && Def.Fill > 0.0f)
		{
			FSlateChildSize Size(ESlateSizeRule::Fill);
			Size.Value = Def.Fill;
			HudSlot->SetSize(Size);
		}
		if (HudSlot && Def.HAlign.IsSet())
		{
			HudSlot->SetHorizontalAlignment(Def.HAlign.GetValue());
		}
	}
	else if (UVerticalBox* Column = Cast<UVerticalBox>(Container))
	{
		UVerticalBoxSlot* HudSlot = AddV(Column, Child.Outer, Def.Margin, Def.HAlign.Get(HAlign_Fill), Def.Fill);
		if (HudSlot && Def.Fill > 0.0f)
		{
			FSlateChildSize Size(ESlateSizeRule::Fill);
			Size.Value = Def.Fill;
			HudSlot->SetSize(Size);
		}
		if (HudSlot && Def.VAlign.IsSet())
		{
			HudSlot->SetVerticalAlignment(Def.VAlign.GetValue());
		}
	}
	else if (UOverlay* Stack = Cast<UOverlay>(Container))
	{
		UOverlaySlot* HudSlot = Stack->AddChildToOverlay(Child.Outer);
		HudSlot->SetPadding(Def.Margin);
		HudSlot->SetHorizontalAlignment(Def.HAlign.Get(HAlign_Fill));
		HudSlot->SetVerticalAlignment(Def.VAlign.Get(VAlign_Fill));
	}
}

// --- Per frame ----------------------------------------------------------------

void UApexHudWidget::SetNodeVisible(FHudNode& Node, bool bVisible)
{
	const int8 Wanted = bVisible ? 1 : 0;
	if (Node.LastVisible != Wanted && Node.Outer)
	{
		Node.LastVisible = Wanted;
		Node.Outer->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UApexHudWidget::ApplyFont(FHudNode& Node, bool bBold)
{
	const int8 Wanted = bBold ? 1 : 0;
	if (Node.LastBold != Wanted)
	{
		Node.LastBold = Wanted;
		if (UTextBlock* Text = Cast<UTextBlock>(Node.Widget))
		{
			Text->SetFont(HudFont(*Node.Def, bBold));
		}
	}
}

void UApexHudWidget::ApplyBrush(FHudNode& Node)
{
	const FApexHudElementDef& Def = *Node.Def;
	if (UBorder* Border = Cast<UBorder>(Node.Widget))
	{
		const bool bPanel = Def.Type == EApexHudElementType::Panel;
		const FLinearColor Fill = bPanel ? (Node.bHasBackground ? Node.LastBackground : FLinearColor::Transparent)
			: (Node.bHasColour ? Node.LastColour : FLinearColor::Transparent);
		const FLinearColor OutlineColour = Node.bHasOutline ? Node.LastOutline : FLinearColor::Transparent;
		Border->SetBrush(MakeBrush(Fill, OutlineColour, Node.bHasOutline ? FMath::Max(Def.OutlineWidth, 1.0f) : 0.0f, Def.Radius));
	}
}

void UApexHudWidget::UpdateNode(int32 NodeIndex, const FApexHudScope& Scope)
{
	FHudNode& Node = Nodes[NodeIndex];
	const FApexHudElementDef& Def = *Node.Def;

	bool bVisible = !Def.Visible.IsSet() || Def.Visible.Expr->Evaluate(Scope).AsBool();
	if (Def.Type == EApexHudElementType::Mirror && bVisible)
	{
		// The director owns the capture; it hands out a texture only while the
		// setting is on and a car is being followed.
		const AApexRaceDirector* Director = AApexRaceDirector::Find(this);
		UTextureRenderTarget2D* Texture = Director ? Director->GetVirtualMirrorTexture() : nullptr;
		if (UApexMirrorWidget* Mirror = Cast<UApexMirrorWidget>(Node.Widget))
		{
			Mirror->SetTexture(Texture);
			// Laid out without a capture, the glass is a faint stand-in.
			Mirror->SetRenderOpacity(Texture || !bEditing ? 1.0f : 0.25f);
		}
		bVisible = Texture != nullptr || bEditing;
	}
	SetNodeVisible(Node, bVisible);
	if (!bVisible)
	{
		return;
	}

	if (Def.FillShare.IsSet() && Node.Outer)
	{
		// A share worked out from the data (a segment of a bar sized by its
		// part of the whole). Never quite zero: a box whose fill shares add up
		// to nothing would divide by it.
		const float Share = FMath::Max(static_cast<float>(Def.FillShare.Expr->Evaluate(Scope).AsNumber()), 0.001f);
		if (Share != Node.LastFill)
		{
			Node.LastFill = Share;
			FSlateChildSize Size(ESlateSizeRule::Fill);
			Size.Value = Share;
			if (UHorizontalBoxSlot* RowSlot = Cast<UHorizontalBoxSlot>(Node.Outer->Slot))
			{
				RowSlot->SetSize(Size);
			}
			else if (UVerticalBoxSlot* ColumnSlot = Cast<UVerticalBoxSlot>(Node.Outer->Slot))
			{
				ColumnSlot->SetSize(Size);
			}
		}
	}

	switch (Def.Type)
	{
	case EApexHudElementType::Text:
	case EApexHudElementType::Label:
	{
		UTextBlock* Text = Cast<UTextBlock>(Node.Widget);
		if (Def.Text.IsSet() && (Def.Text.IsDynamic() || !Node.bHasText))
		{
			FString Value = Def.Text.Expr->Evaluate(Scope).AsString();
			if (Def.bUpper)
			{
				Value = Value.ToUpper();
			}
			if (!Node.bHasText || !Value.Equals(Node.LastText, ESearchCase::CaseSensitive))
			{
				Node.bHasText = true;
				Node.LastText = Value;
				Text->SetText(FText::FromString(Value));
			}
		}
		if (HudUpdateColour(Def.Colour, Scope, Node.bHasColour, Node.LastColour))
		{
			Text->SetColorAndOpacity(FSlateColor(Node.LastColour));
		}
		if (Def.Bold.IsSet() ? (Def.Bold.IsDynamic() || Node.LastBold < 0) : Node.LastBold < 0)
		{
			ApplyFont(Node, Def.Bold.IsSet() && Def.Bold.Expr->Evaluate(Scope).AsBool());
		}
		break;
	}
	case EApexHudElementType::Rect:
	case EApexHudElementType::Panel:
	{
		const bool bFill = Def.Type == EApexHudElementType::Panel
			? HudUpdateColour(Def.Background, Scope, Node.bHasBackground, Node.LastBackground)
			: HudUpdateColour(Def.Colour, Scope, Node.bHasColour, Node.LastColour);
		const bool bOutline = HudUpdateColour(Def.Outline, Scope, Node.bHasOutline, Node.LastOutline);
		if (bFill || bOutline)
		{
			ApplyBrush(Node);
		}
		break;
	}
	case EApexHudElementType::Bar:
	{
		UProgressBar* Bar = Cast<UProgressBar>(Node.Widget);
		if (Def.Value.IsSet())
		{
			const float Value = FMath::Clamp(static_cast<float>(Def.Value.Expr->Evaluate(Scope).AsNumber()), 0.0f, 1.0f);
			if (Value != Node.LastValue)
			{
				Node.LastValue = Value;
				Bar->SetPercent(Value);
			}
		}
		const bool bFill = HudUpdateColour(Def.Colour, Scope, Node.bHasColour, Node.LastColour);
		const bool bTrack = HudUpdateColour(Def.Background, Scope, Node.bHasBackground, Node.LastBackground);
		if (bFill || bTrack)
		{
			FProgressBarStyle Style = Bar->GetWidgetStyle();
			Style.SetBackgroundImage(MakeBrush(Node.bHasBackground ? Node.LastBackground : Palette::Surface));
			Style.SetFillImage(MakeBrush(Node.bHasColour ? Node.LastColour : Palette::Accent));
			Bar->SetWidgetStyle(Style);
		}
		break;
	}
	case EApexHudElementType::Image:
		if (HudUpdateColour(Def.Colour, Scope, Node.bHasColour, Node.LastColour))
		{
			Cast<UImage>(Node.Widget)->SetColorAndOpacity(Node.LastColour);
		}
		break;
	case EApexHudElementType::Minimap:
		if (UApexMinimapWidget* Minimap = Cast<UApexMinimapWidget>(Node.Widget))
		{
			HudUpdateColour(Def.Colour, Scope, Node.bHasColour, Node.LastColour);
			if (UApexHudDataSubsystem* Hud = GetHudData())
			{
				if (!Minimap->HasCenterline())
				{
					// The outline arrives with the lobby state, so keep asking until it has.
					const TArray<FVector2D>& TrackOutline = Hud->GetTrackOutline();
					if (TrackOutline.Num() > 1)
					{
						Minimap->SetCenterline(TrackOutline);
					}
				}
				TArray<FApexMinimapBlip> Blips;
				Hud->MakeMinimapBlips(Blips, Node.bHasColour ? Node.LastColour : Palette::Accent);
				Minimap->SetBlips(MoveTemp(Blips));
			}
		}
		break;
	default:
		break;
	}

	UpdateChildren(NodeIndex, Scope);
}

void UApexHudWidget::UpdateChildren(int32 NodeIndex, const FApexHudScope& Scope)
{
	// The window a list repeat shows, worked out at its first copy and used by the rest.
	const TArray<FApexHudRecord>* List = nullptr;
	int32 Start = 0;

	for (const int32 ChildIndex : Nodes[NodeIndex].Children)
	{
		FHudNode& Child = Nodes[ChildIndex];
		const FApexHudRepeat& Repeat = Child.Def->Repeat;
		if (!Repeat.IsSet())
		{
			UpdateNode(ChildIndex, Scope);
			continue;
		}

		FApexHudScope Inner = Scope;
		if (!Repeat.IsList())
		{
			Inner.Index = Child.Slot;
			UpdateNode(ChildIndex, Inner);
			continue;
		}

		if (Child.Slot == 0)
		{
			List = Scope.Data ? Scope.Data->FindList(Repeat.List) : nullptr;
			Start = 0;
			// Rows around the focus rather than the top of the list: in
			// eleventh place the leaders are not who you are racing.
			if (List && !Repeat.Focus.IsNone() && List->Num() > Repeat.Max)
			{
				const FName Focus = Repeat.Focus;
				const int32 Focused = List->IndexOfByPredicate([Focus](const FApexHudRecord& Row)
				{
					const FApexHudValue* Value = Row.Find(Focus);
					return Value && Value->AsBool();
				});
				if (Focused >= 0)
				{
					Start = FMath::Clamp(Focused - Repeat.Max / 2, 0, List->Num() - Repeat.Max);
				}
			}
		}
		const int32 Row = Start + Child.Slot;
		if (!List || !List->IsValidIndex(Row))
		{
			SetNodeVisible(Child, false);
			continue;
		}
		Inner.Item = &(*List)[Row];
		Inner.Index = Row;
		UpdateNode(ChildIndex, Inner);
	}
}

// --- The HUD editor ---------------------------------------------------------------

void UApexHudWidget::BeginEditing()
{
	if (bEditing)
	{
		return;
	}
	bEditing = true;
	if (UApexHudDataSubsystem* Hud = GetHudData())
	{
		Hud->SetPreview(true);
	}
	// Outside a race the preview has its own outline for the map.
	if (!bRaceActive)
	{
		for (UApexMinimapWidget* Minimap : Minimaps)
		{
			if (Minimap)
			{
				Minimap->SetCenterline(TArray<FVector2D>());
			}
		}
	}
	ApplyVisibility();
}

void UApexHudWidget::EndEditing()
{
	if (!bEditing)
	{
		return;
	}
	bEditing = false;
	if (UApexHudDataSubsystem* Hud = GetHudData())
	{
		Hud->SetPreview(false);
	}
	if (!bRaceActive)
	{
		for (UApexMinimapWidget* Minimap : Minimaps)
		{
			if (Minimap)
			{
				Minimap->SetCenterline(TArray<FVector2D>());
			}
		}
	}
	// The mirror's stand-in goes now; components the editor showed regardless
	// go back to their own rules on the next frame.
	for (UApexMirrorWidget* Mirror : Mirrors)
	{
		if (Mirror)
		{
			Mirror->SetRenderOpacity(1.0f);
		}
	}
	ApplyVisibility();
}

void UApexHudWidget::SetLayout(const FApexHudLayout& InLayout)
{
	Layout = InLayout;
	BuildHud();
	ApplyVisibility();
}

FString UApexHudWidget::GetLayoutFile() const
{
	return LayoutFile.IsEmpty() ? FApexHudLayout::DefaultFile() : LayoutFile;
}

bool UApexHudWidget::IsComponentShown(int32 Index) const
{
	return ComponentRoots.IsValidIndex(Index) && ComponentRoots[Index] != INDEX_NONE;
}

FVector2D UApexHudWidget::GetHudSize() const
{
	return GetCachedGeometry().GetLocalSize();
}

FVector2D UApexHudWidget::AbsoluteToHud(const FVector2D& Absolute) const
{
	return GetCachedGeometry().AbsoluteToLocal(Absolute);
}

FVector2D UApexHudWidget::HudToAbsolute(const FVector2D& Local) const
{
	return GetCachedGeometry().LocalToAbsolute(Local);
}

bool UApexHudWidget::GetComponentRect(int32 Index, FSlateRect& OutRect) const
{
	if (!IsComponentShown(Index) || !ComponentWrappers.IsValidIndex(Index) || !ComponentWrappers[Index])
	{
		return false;
	}
	const UScaleBox* Wrapper = ComponentWrappers[Index];
	const FGeometry& Geometry = Wrapper->GetCachedGeometry();
	const FVector2D AbsoluteSize = Geometry.GetAbsoluteSize();
	if (AbsoluteSize.X < 1.0 || AbsoluteSize.Y < 1.0)
	{
		return false;
	}
	const FVector2D TopLeft = AbsoluteToHud(Geometry.GetAbsolutePosition());
	const FVector2D BottomRight = AbsoluteToHud(Geometry.GetAbsolutePosition() + AbsoluteSize);
	OutRect = FSlateRect(TopLeft, BottomRight);
	return true;
}

void UApexHudWidget::PinAll()
{
	const FVector2D Size = GetHudSize();
	if (Size.X < 1.0 || Size.Y < 1.0)
	{
		return;
	}
	bool bChanged = false;
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		const FString& Id = Components[Index].Id;
		const FApexHudPlacement* Existing = Layout.Find(Id);
		FSlateRect Rect;
		if ((Existing && Existing->bPinned) || !GetComponentRect(Index, Rect))
		{
			continue;
		}
		Layout.Components.Add(Id, ApexHudPlace::PinRect(Rect, Size, Layout.ScaleOf(Id)));
		bChanged = true;
	}
	if (bChanged)
	{
		BuildHud();
		ApplyVisibility();
	}
}

void UApexHudWidget::PlaceComponent(int32 Index, const FApexHudPlacement& Placement)
{
	if (!Components.IsValidIndex(Index))
	{
		return;
	}
	const FString& Id = Components[Index].Id;
	const bool bWasShown = IsComponentShown(Index);
	Layout.Components.Add(Id, Placement);

	UCanvasPanelSlot* PinSlot = ComponentCanvasSlots.IsValidIndex(Index) ? ComponentCanvasSlots[Index] : nullptr;
	UScaleBox* Wrapper = ComponentWrappers.IsValidIndex(Index) ? ComponentWrappers[Index] : nullptr;
	if (bWasShown && Placement.bEnabled && Placement.bPinned && PinSlot && Wrapper)
	{
		// Already on the canvas: move it there, no rebuild, so a drag stays smooth.
		PinSlot->SetAnchors(FAnchors(Placement.Anchor.X, Placement.Anchor.Y));
		PinSlot->SetAlignment(Placement.Anchor);
		PinSlot->SetPosition(Placement.Position);
		Wrapper->SetUserSpecifiedScale(Placement.Scale);
		return;
	}
	BuildHud();
	ApplyVisibility();
}
