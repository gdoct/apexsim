#include "ApexSimInputModule.h"

#include "ApexSimInputLog.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/ApexDirectInputDevice.h"
#endif

DEFINE_LOG_CATEGORY(LogApexInput);

IMPLEMENT_MODULE(FApexSimInputModule, ApexSimInput)

using namespace ApexDirectInput;

namespace
{
	/** What a device's bindings follow: one file, next to the save games. */
	const TCHAR* const RecordsFileName = TEXT("ApexInputDevices.json");

	FAutoConsoleCommand DumpDevicesCommand(
		TEXT("apexsim.input.Devices"),
		TEXT("List the attached DirectInput devices, their slots and their current readings."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (FApexSimInputModule* Module = FApexSimInputModule::Get())
			{
				Module->DumpDevicesToLog();
			}
		}));

	FAutoConsoleCommand RescanDevicesCommand(
		TEXT("apexsim.input.Rescan"),
		TEXT("Enumerate DirectInput devices again, as a plug or unplug does."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			if (FApexSimInputModule* Module = FApexSimInputModule::Get())
			{
				Module->RequestRescan();
			}
		}));
}

FApexSimInputModule* FApexSimInputModule::Get()
{
	return FModuleManager::GetModulePtr<FApexSimInputModule>(TEXT("ApexSimInput"));
}

void FApexSimInputModule::StartupModule()
{
	// Registers the modular feature the platform application asks for; without
	// it nothing would ever create the device.
	IInputDeviceModule::StartupModule();

	// Before anything can read a binding: a key whose name is not registered
	// is an invalid key, and the settings slot is loaded with the game instance.
	RegisterKeys();
	LoadRecords();
}

void FApexSimInputModule::ShutdownModule()
{
#if PLATFORM_WINDOWS
	if (Device.IsValid())
	{
		// The platform application still holds a reference; releasing the
		// hardware here means it is let go before this module's code can be
		// unloaded from under it.
		Device->Shutdown();
		Device.Reset();
	}
#endif
	IModularFeatures::Get().UnregisterModularFeature(IInputDeviceModule::GetModularFeatureName(), this);
}

TSharedPtr<IInputDevice> FApexSimInputModule::CreateInputDevice(
	const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
#if PLATFORM_WINDOWS
	if (Device.IsValid())
	{
		// Two devices would fight over exclusive access to the same wheel.
		UE_LOG(LogApexInput, Warning, TEXT("DirectInput: a device already exists; the second request is ignored"));
		return nullptr;
	}
	Device = MakeShared<FApexDirectInputDevice>(InMessageHandler, *this);
	return Device;
#else
	return nullptr;
#endif
}

const TArray<FDeviceInfo>& FApexSimInputModule::GetDevices() const
{
#if PLATFORM_WINDOWS
	if (Device.IsValid())
	{
		return Device->GetDevices();
	}
#endif
	static const TArray<FDeviceInfo> None;
	return None;
}

const FDeviceInfo* FApexSimInputModule::FindDevice(int32 Slot) const
{
	return GetDevices().FindByPredicate([Slot](const FDeviceInfo& Device) { return Device.Slot == Slot; });
}

const FSlotRecord* FApexSimInputModule::FindRecord(int32 Slot) const
{
	return Records.FindByPredicate([Slot](const FSlotRecord& Record) { return Record.Slot == Slot; });
}

float FApexSimInputModule::GetControlValue(const FControl& Control) const
{
#if PLATFORM_WINDOWS
	if (Device.IsValid() && Control.IsValid())
	{
		return Device->GetControlValue(Control);
	}
#endif
	return 0.0f;
}

void FApexSimInputModule::SetWheelEffects(int32 Slot, const FApexWheelEffects& Effects)
{
#if PLATFORM_WINDOWS
	if (Device.IsValid())
	{
		Device->SetWheelEffects(Slot, Effects);
	}
#endif
}

void FApexSimInputModule::RequestRescan()
{
#if PLATFORM_WINDOWS
	if (Device.IsValid())
	{
		Device->RequestRescan(0.0);
	}
#endif
}

void FApexSimInputModule::DumpDevicesToLog() const
{
#if PLATFORM_WINDOWS
	if (Device.IsValid())
	{
		Device->DumpToLog();
	}
	else
#endif
	{
		UE_LOG(LogApexInput, Display, TEXT("DirectInput: no device layer on this platform"));
	}

	for (const FSlotRecord& Record : Records)
	{
		UE_LOG(LogApexInput, Display, TEXT("  remembered slot %d: \"%s\" (%s)%s"),
			Record.Slot + 1, *Record.Name, KindName(Record.Kind),
			FindDevice(Record.Slot) ? TEXT(" — attached") : TEXT(""));
	}
}

FString FApexSimInputModule::GetDeviceTag(int32 Slot) const
{
	const FDeviceInfo* Attached = FindDevice(Slot);
	const FSlotRecord* Record = FindRecord(Slot);
	if (!Attached && !Record)
	{
		return FString::Printf(TEXT("DEV%d"), Slot + 1);
	}

	const EDeviceKind Kind = Attached ? Attached->Kind : Record->Kind;
	const FString Tag = KindTag(Kind);

	// Two wheels, or two identical button boxes: the tag alone would not say
	// which one a binding is on, so it carries the slot.
	const bool bAmbiguous = GetDevices().ContainsByPredicate([Slot, Kind](const FDeviceInfo& Other)
	{
		return Other.Slot != Slot && Other.Kind == Kind;
	});
	return bAmbiguous ? Tag + FString::FromInt(Slot + 1) : Tag;
}

FString FApexSimInputModule::GetShortName(const FKey& Key) const
{
	const FControl Control = ParseKey(Key);
	if (!Control.IsValid())
	{
		return FString();
	}
	return FString::Printf(TEXT("%s %s"), *GetDeviceTag(Control.Slot), *ControlName(Control));
}

FString FApexSimInputModule::GetLongName(const FKey& Key) const
{
	const FControl Control = ParseKey(Key);
	if (!Control.IsValid())
	{
		return FString();
	}

	const FDeviceInfo* Attached = FindDevice(Control.Slot);
	const FSlotRecord* Record = FindRecord(Control.Slot);
	FString Name = Attached ? Attached->Name : (Record ? Record->Name : FString());
	if (Name.IsEmpty())
	{
		Name = FString::Printf(TEXT("Device %d"), Control.Slot + 1);
	}
	return FString::Printf(TEXT("%s · %s"), *Name, *ControlName(Control));
}

void FApexSimInputModule::NotifyDevicesChanged()
{
	++DevicesSerial;
	DevicesChanged.Broadcast();
}

FString FApexSimInputModule::GetRecordsPath() const
{
	return FPaths::ProjectSavedDir() / RecordsFileName;
}

void FApexSimInputModule::LoadRecords()
{
	FString Json;
	if (FFileHelper::LoadFileToString(Json, *GetRecordsPath()))
	{
		Records = RecordsFromJson(Json);
	}
}

void FApexSimInputModule::SaveRecords() const
{
	const FString Path = GetRecordsPath();
	if (!FFileHelper::SaveStringToFile(RecordsToJson(Records), *Path))
	{
		UE_LOG(LogApexInput, Warning, TEXT("DirectInput: could not write %s; device slots will be worked out again next run"), *Path);
	}
}
