#include "UI/ApexConnectDialogWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/ApexButtonWidget.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexRootWidget.h"
#include "UI/ApexUIStyle.h"

namespace
{
	const FName ActionConnectBack(TEXT("__back"));
	const FName ActionConnect(TEXT("__connect"));
	const FName ActionAdvanced(TEXT("__advanced"));
	const FName ActionRecent(TEXT("__recent"));

	constexpr float ConnectCardWidth = 820.0f;

	/** One labelled field in the form. */
	UWidget* MakeField(UWidgetTree& Tree, const FString& Label, UEditableTextBox*& OutBox, const FString& Hint)
	{
		UVerticalBox* Box = Tree.ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Box, ApexUI::MakeLabel(Tree, Label));

		OutBox = ApexUI::MakeSearchBox(Tree, Hint);
		ApexUI::AddV(Box, ApexUI::MakeSized(Tree, OutBox, -1.0f, 44.0f), FMargin(0.0f, 8.0f, 0.0f, 0.0f));
		return Box;
	}
}

UApexConnectDialogWidget::UApexConnectDialogWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UApexConnectDialogWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
}

void UApexConnectDialogWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.AddDynamic(this, &UApexConnectDialogWidget::HandleConnectionStateChanged);
	}
}

void UApexConnectDialogWidget::NativeDestruct()
{
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnConnectionStateChanged.RemoveDynamic(this, &UApexConnectDialogWidget::HandleConnectionStateChanged);
	}
	Super::NativeDestruct();
}

void UApexConnectDialogWidget::OnScreenActivated()
{
	Super::OnScreenActivated();

	if (const UApexMenuFlowSubsystem* Flow = GetFlow())
	{
		if (HostField)  { HostField->SetText(FText::FromString(Flow->ServerHost)); }
		// FromInt, not AsNumber: a port is an identifier, and localised grouping
		// turns 9000 into "9,000", which then fails to parse back.
		if (PortField)  { PortField->SetText(FText::FromString(FString::FromInt(Flow->ServerPort))); }
		if (NameField)  { NameField->SetText(FText::FromString(Flow->PlayerName)); }
		if (TokenField) { TokenField->SetText(FText::FromString(Flow->AuthToken)); }

		if (RecentButton)
		{
			RecentButton->SetLabel(FString::Printf(TEXT("%s:%d"), *Flow->ServerHost, Flow->ServerPort));
		}
	}

	RefreshStatus();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void UApexConnectDialogWidget::BuildLayout()
{
	UVerticalBox* Card = WidgetTree->ConstructWidget<UVerticalBox>();

	ApexUI::AddV(Card, ApexUI::MakeLabel(*WidgetTree, TEXT("Multiplayer"), ApexUI::Palette::Accent));
	ApexUI::AddV(
		Card,
		ApexUI::MakeText(*WidgetTree, TEXT("Connect to a server"), ApexUI::Font::Display(42.0f), ApexUI::Palette::TextPrimary),
		FMargin(0.0f, 10.0f, 0.0f, 26.0f));

	ApexUI::AddV(Card, BuildRecentPanel(), FMargin(0.0f, 0.0f, 0.0f, 26.0f));
	ApexUI::AddV(Card, BuildFields());

	// Advanced: the auth token. TLS has no client-side switch — the transport
	// negotiates it — so this row is the token alone.
	FApexButtonSpec AdvancedSpec;
	AdvancedSpec.Label = TEXT("+ Advanced (join token)");
	AdvancedSpec.Variant = EApexButtonVariant::Bare;
	AdvancedSpec.LabelSize = 14.0f;
	AdvancedSpec.LabelColour = ApexUI::Palette::Accent;
	AdvancedSpec.ActionId = ActionAdvanced;

	UApexButtonWidget* AdvancedToggle = WidgetTree->ConstructWidget<UApexButtonWidget>();
	AdvancedToggle->Setup(AdvancedSpec);
	AdvancedToggle->OnActivated.AddDynamic(this, &UApexConnectDialogWidget::HandleButtonActivated);
	ApexUI::AddV(Card, AdvancedToggle, FMargin(0.0f, 16.0f, 0.0f, 0.0f), HAlign_Left);

	// Status line with a light, mirroring the shell's own connection indicator.
	UHorizontalBox* StatusRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	UBorder* Dot = nullptr;
	ApexUI::AddH(StatusRow, ApexUI::MakeDot(*WidgetTree, ApexUI::Palette::TextMuted, 9.0f, &Dot), FMargin(0.0f, 0.0f, 10.0f, 0.0f));
	StatusDot = Dot;
	ConnectStatusText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextMuted);
	ApexUI::AddH(StatusRow, ConnectStatusText);
	ApexUI::AddV(Card, StatusRow, FMargin(0.0f, 22.0f, 0.0f, 0.0f));

	// Actions.
	FApexButtonSpec BackSpec;
	BackSpec.Label = TEXT("Back");
	BackSpec.Variant = EApexButtonVariant::Ghost;
	BackSpec.bCentreLabel = true;
	BackSpec.LabelSize = 17.0f;
	BackSpec.Height = 56.0f;
	BackSpec.ActionId = ActionConnectBack;

	UApexButtonWidget* BackButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	BackButton->Setup(BackSpec);
	BackButton->OnActivated.AddDynamic(this, &UApexConnectDialogWidget::HandleButtonActivated);

	FApexButtonSpec ConnectSpec;
	ConnectSpec.Label = TEXT("Connect");
	ConnectSpec.KeyCap = TEXT("Enter");
	ConnectSpec.Variant = EApexButtonVariant::Primary;
	ConnectSpec.LabelSize = 22.0f;
	ConnectSpec.Height = 56.0f;
	ConnectSpec.ActionId = ActionConnect;

	ConnectAction = WidgetTree->ConstructWidget<UApexButtonWidget>();
	ConnectAction->Setup(ConnectSpec);
	ConnectAction->OnActivated.AddDynamic(this, &UApexConnectDialogWidget::HandleButtonActivated);

	UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Actions, ApexUI::MakeSized(*WidgetTree, BackButton, 180.0f, -1.0f), FMargin(0.0f, 0.0f, 12.0f, 0.0f));
	ApexUI::AddH(Actions, ConnectAction, FMargin(), VAlign_Fill, 1.0f);
	ApexUI::AddV(Card, Actions, FMargin(0.0f, 18.0f, 0.0f, 0.0f));

	UBorder* Panel = ApexUI::MakePanel(
		*WidgetTree,
		Card,
		FMargin(44.0f, 40.0f),
		ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));

	// Centred on the page: this is a dialog, not a full screen.
	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	ApexUI::AddV(Centre, ApexUI::MakeSized(*WidgetTree, Panel, ConnectCardWidth, -1.0f), FMargin(), HAlign_Center);
	ApexUI::AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.2f);

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		Centre,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Background));
}

UWidget* UApexConnectDialogWidget::BuildRecentPanel()
{
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Box, ApexUI::MakeLabel(*WidgetTree, TEXT("Last server you used")), FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	FApexButtonSpec Spec;
	Spec.Label = TEXT("—");
	Spec.SubLabel = TEXT("Connect straight back");
	Spec.Variant = EApexButtonVariant::Panel;
	Spec.LabelSize = 19.0f;
	Spec.Height = 70.0f;
	Spec.ActionId = ActionRecent;

	RecentButton = WidgetTree->ConstructWidget<UApexButtonWidget>();
	RecentButton->Setup(Spec);
	RecentButton->OnActivated.AddDynamic(this, &UApexConnectDialogWidget::HandleButtonActivated);
	ApexUI::AddV(Box, RecentButton);

	return Box;
}

UWidget* UApexConnectDialogWidget::BuildFields()
{
	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();

	UEditableTextBox* Host = nullptr;
	UEditableTextBox* Port = nullptr;
	UEditableTextBox* Name = nullptr;
	UEditableTextBox* Token = nullptr;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	ApexUI::AddH(Row, MakeField(*WidgetTree, TEXT("Host"), Host, TEXT("127.0.0.1")), FMargin(), VAlign_Fill, 1.6f);
	ApexUI::AddH(Row, MakeField(*WidgetTree, TEXT("Port"), Port, TEXT("9000")), FMargin(14.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.0f);
	ApexUI::AddH(Row, MakeField(*WidgetTree, TEXT("Driver name"), Name, TEXT("Player")), FMargin(14.0f, 0.0f, 0.0f, 0.0f), VAlign_Fill, 1.6f);
	ApexUI::AddV(Stack, Row);

	TokenRow = MakeField(*WidgetTree, TEXT("Join token"), Token, TEXT("dev-token"));
	TokenRow->SetVisibility(ESlateVisibility::Collapsed);
	ApexUI::AddV(Stack, TokenRow, FMargin(0.0f, 14.0f, 0.0f, 0.0f));

	HostField = Host;
	PortField = Port;
	NameField = Name;
	TokenField = Token;

	// Enter from any field connects, the way a login form behaves. The field
	// consumes the key itself, so this is the only place to hear about it.
	for (UEditableTextBox* Field : { Host, Port, Name, Token })
	{
		Field->OnTextCommitted.AddDynamic(this, &UApexConnectDialogWidget::HandleFieldCommitted);
	}

	return Stack;
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

void UApexConnectDialogWidget::RefreshStatus()
{
	const UApexNetSubsystem* Net = GetNet();
	if (!Net || !ConnectStatusText)
	{
		return;
	}

	FString Text;
	FLinearColor Colour = ApexUI::Palette::TextMuted;

	switch (Net->GetConnectionState())
	{
	case EApexConnectionState::Authenticated:
	{
		// Ping is -1 until the first heartbeat round trip completes, a couple of
		// seconds after connecting.
		const int32 Ping = Net->GetPingMs();
		Text = Ping >= 0
			? FString::Printf(TEXT("Connected as %s · %d ms"), *Net->GetPlayerName(), Ping)
			: FString::Printf(TEXT("Connected as %s"), *Net->GetPlayerName());
		Colour = ApexUI::Palette::Live;
		break;
	}
	case EApexConnectionState::Connecting:
		Text = TEXT("Connecting…");
		Colour = ApexUI::Palette::Accent;
		break;
	case EApexConnectionState::Authenticating:
		Text = TEXT("Authenticating…");
		Colour = ApexUI::Palette::Accent;
		break;
	case EApexConnectionState::Failed:
		Text = TEXT("Could not connect — check the address and that the server is running");
		Colour = ApexUI::Palette::Error;
		break;
	case EApexConnectionState::Reconnecting:
		Text = TEXT("Server unreachable — retrying automatically. Start the server, or connect to a different address");
		Colour = ApexUI::Palette::Error;
		break;
	default:
		Text = TEXT("Not connected");
		break;
	}

	ConnectStatusText->SetText(FText::FromString(Text));
	ConnectStatusText->SetColorAndOpacity(FSlateColor(Colour));
	ApexUI::SetDotColour(StatusDot, Colour);
}

void UApexConnectDialogWidget::Connect()
{
	UApexMenuFlowSubsystem* Flow = GetFlow();
	UApexNetSubsystem* Net = GetNet();
	if (!Flow || !Net)
	{
		return;
	}

	const FString Host = HostField ? HostField->GetText().ToString().TrimStartAndEnd() : Flow->ServerHost;
	const FString PortText = PortField ? PortField->GetText().ToString().TrimStartAndEnd() : FString();
	const FString Name = NameField ? NameField->GetText().ToString().TrimStartAndEnd() : Flow->PlayerName;
	const FString Token = TokenField ? TokenField->GetText().ToString().TrimStartAndEnd() : Flow->AuthToken;

	if (Host.IsEmpty())
	{
		ShowToast(TEXT("Enter a host address"), true);
		return;
	}

	const int32 Port = PortText.IsNumeric() ? FCString::Atoi(*PortText) : 0;
	if (Port <= 0 || Port > 65535)
	{
		ShowToast(TEXT("Port must be a number between 1 and 65535"), true);
		return;
	}

	Flow->ServerHost = Host;
	Flow->ServerPort = Port;
	Flow->PlayerName = Name.IsEmpty() ? TEXT("Player") : Name;
	Flow->AuthToken = Token.IsEmpty() ? TEXT("dev-token") : Token;
	// Remembered so the next launch connects here without being asked.
	Flow->SaveProfile();

	UE_LOG(LogApexSim, Log, TEXT("Connecting to %s:%d as '%s'"), *Flow->ServerHost, Flow->ServerPort, *Flow->PlayerName);
	bConnectRequested = true;
	Net->Connect(Flow->ServerHost, Flow->ServerPort, Flow->PlayerName, Flow->AuthToken);
	RefreshStatus();
}

void UApexConnectDialogWidget::HandleButtonActivated(UApexButtonWidget* Button)
{
	if (!Button)
	{
		return;
	}

	const FName Id = Button->GetActionId();

	if (Id == ActionConnectBack)
	{
		GoBack();
	}
	else if (Id == ActionConnect || Id == ActionRecent)
	{
		Connect();
	}
	else if (Id == ActionAdvanced && TokenRow)
	{
		bAdvancedShown = !bAdvancedShown;
		TokenRow->SetVisibility(bAdvancedShown ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		Button->SetLabel(bAdvancedShown ? TEXT("− Advanced (join token)") : TEXT("+ Advanced (join token)"));
	}
}

void UApexConnectDialogWidget::HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail)
{
	RefreshStatus();

	// Only leave on a connect this screen asked for. The shell auto-connects on
	// startup, and that must not eject someone who opened this screen to change
	// the address while already connected.
	if (NewState == EApexConnectionState::Authenticated
		&& bConnectRequested
		&& GetRoot()
		&& GetRoot()->GetCurrentScreen() == EApexScreen::ConnectDialog)
	{
		bConnectRequested = false;
		GetRoot()->ReplaceScreen(EApexScreen::MainMenu);
	}
}

void UApexConnectDialogWidget::HandleFieldCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter)
	{
		Connect();
	}
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool UApexConnectDialogWidget::IsEditingField() const
{
	for (const UEditableTextBox* Field : { HostField.Get(), PortField.Get(), NameField.Get(), TokenField.Get() })
	{
		if (Field && Field->HasKeyboardFocus())
		{
			return true;
		}
	}
	return false;
}

void UApexConnectDialogWidget::FocusDefault()
{
	if (!ApexNav::Focus(RecentButton) && !ApexNav::Focus(HostField))
	{
		Super::FocusDefault();
	}
}

bool UApexConnectDialogWidget::HandleAccept()
{
	// Accept that reached the screen — nothing below took it — connects.
	Connect();
	return true;
}

bool UApexConnectDialogWidget::HandleBack()
{
	// Back inside a field leaves the field, not the screen.
	if (IsEditingField())
	{
		if (!ApexNav::Focus(ConnectAction))
		{
			FocusDefault();
		}
		return true;
	}
	return Super::HandleBack();
}
