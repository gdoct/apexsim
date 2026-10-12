#include "Race/ApexSpectatorView.h"
#include "Input/ApexInputConfig.h"

#include "Race/ApexRaceCoordinate.h"

namespace ApexSpectate
{
	ECamera NextCamera(ECamera Camera)
	{
		return static_cast<ECamera>((static_cast<uint8>(Camera) + 1) % static_cast<uint8>(ECamera::Count));
	}

	const TCHAR* CameraName(ECamera Camera)
	{
		switch (Camera)
		{
		case ECamera::Broadcast: return TEXT("TV");
		case ECamera::Chase: return TEXT("CHASE");
		case ECamera::Onboard: return TEXT("ONBOARD");
		default: return TEXT("");
		}
	}

	ETowerMode NextTowerMode(ETowerMode Mode)
	{
		return static_cast<ETowerMode>((static_cast<uint8>(Mode) + 1) % static_cast<uint8>(ETowerMode::Count));
	}

	const TCHAR* TowerModeKey(ETowerMode Mode)
	{
		switch (Mode)
		{
		case ETowerMode::Interval: return TEXT("interval");
		case ETowerMode::Gap: return TEXT("gap");
		case ETowerMode::LastLap: return TEXT("last");
		case ETowerMode::BestLap: return TEXT("best");
		case ETowerMode::Tyres: return TEXT("tyres");
		default: return TEXT("");
		}
	}

	TArray<int32> RaceOrder(TConstArrayView<FRunner> Runners)
	{
		TArray<FRunner> Sorted(Runners.GetData(), Runners.Num());
		Sorted.Sort([](const FRunner& A, const FRunner& B)
		{
			if (A.FinishPosition != B.FinishPosition || !FMath::IsNearlyEqual(A.RaceDistanceM, B.RaceDistanceM))
			{
				return ApexRace::RanksAhead(A.FinishPosition, A.RaceDistanceM, B.FinishPosition, B.RaceDistanceM);
			}
			return A.CarIndex < B.CarIndex;
		});
		TArray<int32> Order;
		Order.Reserve(Sorted.Num());
		for (const FRunner& Runner : Sorted)
		{
			Order.Add(Runner.CarIndex);
		}
		return Order;
	}

	int32 Step(const TArray<int32>& Order, int32 Current, int32 Delta)
	{
		if (Order.Num() == 0)
		{
			return INDEX_NONE;
		}
		const int32 At = Order.IndexOfByKey(Current);
		if (At == INDEX_NONE)
		{
			return Order[0];
		}
		const int32 Count = Order.Num();
		return Order[((At + Delta) % Count + Count) % Count];
	}

	int32 AtPosition(const TArray<int32>& Order, int32 Position)
	{
		return Order.IsValidIndex(Position - 1) ? Order[Position - 1] : INDEX_NONE;
	}

	FName ActionIdOf(EAction Action)
	{
		using namespace ApexInput::Actions;
		switch (Action)
		{
		case EAction::PreviousCar:   return WatchPreviousCar;
		case EAction::NextCar:       return WatchNextCar;
		case EAction::Camera:        return WatchCamera;
		case EAction::Auto:          return WatchAuto;
		case EAction::Tower:         return WatchTower;
		case EAction::Overlay:       return WatchOverlay;
		case EAction::NextRace:      return WatchNextRace;
		case EAction::Leave:         return WatchLeave;
		case EAction::PlayPause:     return WatchPlayPause;
		case EAction::SeekBack:      return WatchSeekBack;
		case EAction::SeekForward:   return WatchSeekForward;
		case EAction::Slower:        return WatchSlower;
		case EAction::Faster:        return WatchFaster;
		case EAction::Weather:       return WatchWeather;
		case EAction::TimeEarlier:   return WatchTimeEarlier;
		case EAction::TimeLater:     return WatchTimeLater;
		case EAction::PreviousTrack: return WatchPreviousTrack;
		case EAction::NextTrack:     return WatchNextTrack;
		default:                     return NAME_None;
		}
	}

	FCommand CommandFor(const FKey& Key, FBoundKey Bound)
	{
		FCommand Command;
		if (!Key.IsValid())
		{
			return Command;
		}
		// What the player has bound, pad and keyboard: the first action
		// that owns the key (the controls screen warns when two do).
		for (uint8 Raw = static_cast<uint8>(EAction::PreviousCar); Raw <= static_cast<uint8>(EAction::NextTrack); ++Raw)
		{
			const EAction Action = static_cast<EAction>(Raw);
			const FName Id = ActionIdOf(Action);
			if (Id.IsNone())
			{
				continue;
			}
			if (Bound(Id, ApexInput::Slot::Gamepad) == Key || Bound(Id, ApexInput::Slot::Keyboard) == Key)
			{
				Command.Action = Action;
				return Command;
			}
		}
		// What is not rebindable: the arrows and the D-pad's vertical beside
		// the bound keys, the numpad's minus and plus beside - and =, and the
		// number row's P1 to P10 (0 is P10).
		if (Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Up)
		{
			Command.Action = EAction::PreviousCar;
		}
		else if (Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Down)
		{
			Command.Action = EAction::NextCar;
		}
		else if (Key == EKeys::Subtract)
		{
			Command.Action = EAction::Slower;
		}
		else if (Key == EKeys::Add)
		{
			Command.Action = EAction::Faster;
		}
		else
		{
			static const FKey Digits[10] = {EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four,
				EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine};
			for (int32 Digit = 0; Digit < 10; ++Digit)
			{
				if (Key == Digits[Digit])
				{
					Command.Action = EAction::Position;
					Command.Position = Digit == 0 ? 10 : Digit;
					break;
				}
			}
		}
		return Command;
	}

	FCommand CommandFor(const FKey& Key)
	{
		return CommandFor(Key, [](FName ActionId, int32 Slot)
		{
			const ApexInput::FSlotDef* Def = ApexInput::FindSlot(ActionId, Slot);
			return Def ? Def->DefaultKey : FKey();
		});
	}

	int32 StepIndex(int32 Count, int32 Index, int32 Direction)
	{
		if (Count <= 0)
		{
			return 0;
		}
		return ((Index + Direction) % Count + Count) % Count;
	}

	EApexWeather StepWeather(EApexWeather Weather, int32 Direction)
	{
		return static_cast<EApexWeather>(StepIndex(FApexSessionConditions::WeatherCount, static_cast<int32>(Weather), Direction));
	}

	int32 StepTimeOfDay(int32 Minutes, int32 Direction)
	{
		const int32 Day = FApexSessionConditions::MinutesPerDay;
		return ((Minutes + 60 * Direction) % Day + Day) % Day;
	}

	float StepPlaybackRate(float Rate, int32 Direction)
	{
		static const float Rates[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
		constexpr int32 Count = 5;
		int32 Nearest = 2;
		for (int32 i = 0; i < Count; ++i)
		{
			if (FMath::Abs(Rates[i] - Rate) < FMath::Abs(Rates[Nearest] - Rate))
			{
				Nearest = i;
			}
		}
		return Rates[FMath::Clamp(Nearest + (Direction > 0 ? 1 : Direction < 0 ? -1 : 0), 0, Count - 1)];
	}
}
