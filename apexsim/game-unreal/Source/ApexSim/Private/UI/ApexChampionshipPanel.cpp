#include "UI/ApexChampionshipPanel.h"

#include "ApexNetSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "UI/ApexEventLabels.h"
#include "UI/ApexUIStyle.h"

namespace ApexChampionshipPanel
{
	bool Fill(UWidgetTree& Tree, UVerticalBox& Box, const UApexNetSubsystem& Net, int32 Top)
	{
		Box.ClearChildren();
		const FApexSessionEvent& Event = Net.GetSessionEvent();
		if (!Event.IsEvent())
		{
			return false;
		}
		const TArray<const FApexChampionship*> Champs = ApexEventLabels::ChampionshipsOfEvent(Net.GetChampionships(), Event.Id);
		if (Champs.IsEmpty())
		{
			return false;
		}

		const FString PlayerName = Net.GetPlayerName();
		bool bFirst = true;
		for (const FApexChampionship* Champ : Champs)
		{
			ApexUI::AddV(&Box, ApexUI::MakeLabel(Tree, TEXT("Championship")), FMargin(0.0f, bFirst ? 0.0f : 18.0f, 0.0f, 8.0f));
			bFirst = false;

			UTextBlock* Name = ApexUI::MakeText(Tree, Champ->Name, ApexUI::Font::Display(18.0f), ApexUI::Palette::TextPrimary);
			Name->SetAutoWrapText(true);
			ApexUI::AddV(&Box, Name);
			ApexUI::AddV(&Box, ApexUI::MakeText(Tree, ApexEventLabels::StandingsCaption(*Champ, Event.Id),
				ApexUI::Font::Mono(9.0f, 120), ApexUI::Palette::TextMuted), FMargin(0.0f, 2.0f, 0.0f, 0.0f));

			FString Notes = ApexEventLabels::FastestLapRule(*Champ);
			if (const FApexChampionshipRound* Round = ApexEventLabels::RoundOf(*Champ, Event.Id))
			{
				const FString Fastest = ApexEventLabels::FastestLapLine(*Champ, *Round);
				if (!Fastest.IsEmpty())
				{
					Notes = Notes.IsEmpty() ? Fastest : Notes + TEXT(" · ") + Fastest;
				}
			}
			if (!Notes.IsEmpty())
			{
				UTextBlock* Line = ApexUI::MakeText(Tree, Notes, ApexUI::Font::Body(12.0f), ApexUI::Palette::TextSecondary);
				Line->SetAutoWrapText(true);
				ApexUI::AddV(&Box, Line, FMargin(0.0f, 4.0f, 0.0f, 0.0f));
			}

			const TArray<int32> Shown = ApexEventLabels::ShownStandings(*Champ, PlayerName, Top);
			if (Shown.IsEmpty())
			{
				ApexUI::AddV(&Box, ApexUI::MakeText(Tree, TEXT("No round raced yet."), ApexUI::Font::Body(13.0f), ApexUI::Palette::TextMuted),
					FMargin(0.0f, 8.0f, 0.0f, 0.0f));
				continue;
			}
			const int32 Own = ApexEventLabels::OwnStanding(*Champ, PlayerName);
			int32 Previous = INDEX_NONE;
			for (const int32 Index : Shown)
			{
				if (Previous != INDEX_NONE && Index > Previous + 1)
				{
					// The player's own line, further down.
					ApexUI::AddV(&Box, ApexUI::MakeText(Tree, TEXT("…"), ApexUI::Font::Mono(10.0f), ApexUI::Palette::TextMuted), FMargin(0.0f, 0.0f, 0.0f, 2.0f));
				}
				Previous = Index;
				const FApexChampionshipStanding& Standing = Champ->Standings[Index];
				const bool bOwn = Index == Own;
				const FLinearColor Colour = bOwn ? ApexUI::Palette::Accent : ApexUI::Palette::TextPrimary;

				UHorizontalBox* Row = Tree.ConstructWidget<UHorizontalBox>();
				ApexUI::AddH(Row, ApexUI::MakeSized(Tree,
					ApexUI::MakeText(Tree, FString::Printf(TEXT("P%d"), Standing.Position > 0 ? Standing.Position : Index + 1), ApexUI::Font::Mono(11.0f), ApexUI::Palette::TextMuted),
					44.0f, -1.0f));
				const FString Who = Standing.bAi ? FString::Printf(TEXT("AI · %s"), *Standing.Name) : Standing.Name;
				UTextBlock* WhoText = ApexUI::MakeText(Tree, Who, ApexUI::Font::Body(14.0f, bOwn), Colour);
				WhoText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
				ApexUI::AddH(Row, WhoText, FMargin(), VAlign_Center, 1.0f);
				if (Standing.Wins > 0 || Standing.FastestLaps > 0)
				{
					TArray<FString> Marks;
					if (Standing.Wins > 0)
					{
						Marks.Add(FString::Printf(TEXT("%dW"), Standing.Wins));
					}
					if (Standing.FastestLaps > 0)
					{
						Marks.Add(FString::Printf(TEXT("%dFL"), Standing.FastestLaps));
					}
					ApexUI::AddH(Row, ApexUI::MakeText(Tree, FString::Join(Marks, TEXT(" ")), ApexUI::Font::Mono(9.0f, 60), ApexUI::Palette::TextMuted),
						FMargin(8.0f, 0.0f));
				}
				ApexUI::AddH(Row, ApexUI::MakeText(Tree, FString::Printf(TEXT("%d PTS"), Standing.Points), ApexUI::Font::Mono(11.0f, 60), Colour));
				ApexUI::AddV(&Box, Row, FMargin(0.0f, Index == Shown[0] ? 8.0f : 0.0f, 0.0f, 3.0f));
			}
		}
		return true;
	}
}
