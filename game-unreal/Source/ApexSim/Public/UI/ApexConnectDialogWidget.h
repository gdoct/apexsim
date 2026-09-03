#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexConnectDialogWidget.generated.h"

class UApexButtonWidget;
class UBorder;
class UEditableTextBox;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * Where a server address is entered.
 *
 * The mockup shows a "found on your network" list; there is no discovery
 * protocol on either side, so this offers the last server that worked instead
 * of inventing neighbours.
 */
UCLASS()
class APEXSIM_API UApexConnectDialogWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexConnectDialogWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;

	// --- Navigation ---------------------------------------------------------
	//
	// A form: the last server on top, three fields in a row, then the actions.
	// Slate's geometric search fits that shape as it is. The fields are the
	// special case — Enter in any of them connects, and Back leaves the field
	// before it leaves the screen.

	/** The last server used, which is what most visits are here to press. */
	virtual void FocusDefault() override;
	virtual bool HandleAccept() override;
	virtual bool HandleBack() override;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	void BuildLayout();
	/** Field stack: host, port, driver name, and the collapsed token row. */
	UWidget* BuildFields();
	UWidget* BuildRecentPanel();

	void RefreshStatus();
	/** Reads the fields into the flow subsystem and starts connecting. */
	void Connect();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail);
	UFUNCTION() void HandleFieldCommitted(const FText& Text, ETextCommit::Type CommitType);

	/** True while the keyboard is inside one of the text fields. */
	bool IsEditingField() const;

	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> HostField;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PortField;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> NameField;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> TokenField;
	UPROPERTY(Transient) TObjectPtr<UWidget> TokenRow;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> RecentButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ConnectAction;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ConnectStatusText;
	UPROPERTY(Transient) TObjectPtr<UBorder> StatusDot;

	bool bAdvancedShown = false;
	/** Set only for a connect this screen started, so background ones are ignored. */
	bool bConnectRequested = false;
};
