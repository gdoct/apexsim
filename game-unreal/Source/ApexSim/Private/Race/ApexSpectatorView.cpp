#include "Race/ApexSpectatorView.h"

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

	FCommand CommandFor(const FKey& Key)
	{
		FCommand Command;
		if (Key == EKeys::Up || Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Up || Key == EKeys::Gamepad_DPad_Left
			|| Key == EKeys::Gamepad_LeftShoulder)
		{
			Command.Action = EAction::PreviousCar;
		}
		else if (Key == EKeys::Down || Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Down || Key == EKeys::Gamepad_DPad_Right
			|| Key == EKeys::Gamepad_RightShoulder)
		{
			Command.Action = EAction::NextCar;
		}
		else if (Key == EKeys::C || Key == EKeys::Gamepad_FaceButton_Top)
		{
			Command.Action = EAction::Camera;
		}
		else if (Key == EKeys::A || Key == EKeys::Gamepad_FaceButton_Left)
		{
			Command.Action = EAction::Auto;
		}
		else if (Key == EKeys::T || Key == EKeys::Gamepad_Special_Left)
		{
			Command.Action = EAction::Tower;
		}
		else if (Key == EKeys::H || Key == EKeys::Gamepad_RightThumbstick)
		{
			Command.Action = EAction::Overlay;
		}
		else if (Key == EKeys::N || Key == EKeys::Gamepad_LeftThumbstick)
		{
			Command.Action = EAction::NextRace;
		}
		else if (Key == EKeys::BackSpace || Key == EKeys::Gamepad_FaceButton_Right)
		{
			Command.Action = EAction::Leave;
		}
		else if (Key == EKeys::SpaceBar || Key == EKeys::Gamepad_FaceButton_Bottom)
		{
			Command.Action = EAction::PlayPause;
		}
		else if (Key == EKeys::Comma || Key == EKeys::Gamepad_LeftTrigger)
		{
			Command.Action = EAction::SeekBack;
		}
		else if (Key == EKeys::Period || Key == EKeys::Gamepad_RightTrigger)
		{
			Command.Action = EAction::SeekForward;
		}
		else if (Key == EKeys::Hyphen || Key == EKeys::Subtract)
		{
			Command.Action = EAction::Slower;
		}
		else if (Key == EKeys::Equals || Key == EKeys::Add)
		{
			Command.Action = EAction::Faster;
		}
		else if (Key == EKeys::W)
		{
			Command.Action = EAction::Weather;
		}
		else if (Key == EKeys::LeftBracket)
		{
			Command.Action = EAction::TimeEarlier;
		}
		else if (Key == EKeys::RightBracket)
		{
			Command.Action = EAction::TimeLater;
		}
		else if (Key == EKeys::PageUp)
		{
			Command.Action = EAction::PreviousTrack;
		}
		else if (Key == EKeys::PageDown)
		{
			Command.Action = EAction::NextTrack;
		}
		else
		{
			// The number row: 1 to 9 are P1 to P9 and 0 is P10.
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
