#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionBrowserWidget.generated.h"

class UApexButtonWidget;
class UApexSegmentedWidget;
class UScrollBox;
class UTextBlock;

/** The session browser's rules, pure for the tests (`ApexSim.UI.SessionBrowser`). */
namespace ApexBrowser
{
	/** What the list shows: the live sessions, some of them, or the server's recordings. */
	enum class EFilter : uint8
	{
		All,
		/** Not full, not over: Enter puts you on the grid or in the lobby. */
		Joinable,
		/** A race being driven: what Watch is for. */
		Racing,
		/** Finished sessions the server recorded (`/recordings`). */
		Recorded,
		Count,
	};

	APEXSIM_API bool Passes(const FApexSessionSummary& Session, EFilter Filter);

	/**
	 * The session's damage rule in its row's line: empty for Full (the
	 * default is not worth the words), else "reduced damage", "no damage"
	 * or "visual damage".
	 */
	APEXSIM_API FString DamageWords(EApexDamageLevel Damage);

	/** One of the server's recordings, as `/recordings` lists it. */
	struct FRecording
	{
		FString Id;
		/** The stream to fetch (`/recordings/<File>`); empty for one from before streams. */
		FString File;
		/** The server's track id: the catalog's display name is shown, never `Track`, the file's own name. */
		FString TrackId;
		FString Track;
		FString Kind;
		int64 RecordedAt = 0;
		double DurationS = 0.0;
		int32 Cars = 0;
		int32 Humans = 0;
		int32 Laps = 0;
		FString Winner;
	};

	/** `/recordings`' JSON, newest first; false when it is not that. */
	APEXSIM_API bool ParseRecordings(const FString& Json, TArray<FRecording>& Out);
}

/**
 * The live sessions on the server (the latest LobbyState), built in C++ in
 * the menu style: one row a session (the track's preview, its name, host,
 * drivers, kind, length and sky, and its state), Enter on a row joins it in
 * the pending car, Watch spectates a race being driven. The header has Back
 * and Refresh; Change car and Create session sit above the list.
 *
 * The list changes often (counts and states move with every broadcast), so
 * rows are rebuilt only when the set of sessions changes and refreshed in
 * place otherwise, keeping the pad's place.
 */
UCLASS()
class APEXSIM_API UApexSessionBrowserWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexSessionBrowserWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	void RefreshList();
	/** The row a session's spec says, for a fresh row or one updated in place. */
	void ApplyRow(int32 Index, const FApexSessionSummary& Session);
	void Join(int32 Index);
	void Watch(int32 Index);
	void Refresh();
	/** Show another filter (the row of pills, F or pad X). */
	void SetFilter(ApexBrowser::EFilter InFilter);
	/** Ask the server's health port for its recordings. */
	void FetchRecordings();
	/** Watch a recording: fetched into the Replays folder first unless it is there already. */
	void WatchRecording(int32 Index);
	void RebuildRecordingRows();
	UFUNCTION() void HandleFilterChosen(UApexSegmentedWidget* Control, int32 Index);

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> HeaderBackButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> RefreshAction;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> ChangeCarButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CreateButton;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CountText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusLine;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CarText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EmptyText;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> List;

	/** Per session: the row (Enter joins) and its Watch button. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> Rows;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> WatchButtons;

	/** The sessions the rows show, in their order. */
	TArray<FApexSessionSummary> Shown;
	int32 LastFocused = 0;

	UPROPERTY(Transient) TObjectPtr<UApexSegmentedWidget> FilterPills;
	ApexBrowser::EFilter Filter = ApexBrowser::EFilter::All;
	/** The server's recordings as last fetched, and whether a fetch or a download is under way. */
	TArray<ApexBrowser::FRecording> Recordings;
	bool bFetchingRecordings = false;
	bool bDownloading = false;
	FString RecordingsNote;
};
