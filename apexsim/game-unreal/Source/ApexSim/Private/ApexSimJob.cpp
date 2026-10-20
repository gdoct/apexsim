#include "ApexSimJob.h"

#include "ApexRepoPaths.h"
#include "ApexSim.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <atomic>

bool ApexSimContent::Find(FString& OutTracksDir, FString& OutCarsDir, FString& OutWhy)
{
	FString Root;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-ApexContentDir="), Root))
	{
		Root = FPlatformProperties::RequiresCookedData()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT(".."), TEXT("Server"), TEXT("content"))
			: ApexRepoPaths::ContentRoot();
	}
	Root = FPaths::ConvertRelativePathToFull(Root);
	OutTracksDir = FPaths::Combine(Root, TEXT("tracks"));
	const FString Cars = FPaths::Combine(Root, TEXT("cars"));
	const FString DefaultCars = FPaths::Combine(Cars, TEXT("default"));
	OutCarsDir = IFileManager::Get().DirectoryExists(*DefaultCars) ? DefaultCars : Cars;
	if (!IFileManager::Get().DirectoryExists(*OutTracksDir) || !IFileManager::Get().DirectoryExists(*OutCarsDir))
	{
		OutWhy = FString::Printf(TEXT("no tracks and cars under %s"), *Root);
		return false;
	}
	return true;
}

FString ApexSimContent::TrackYaml(const FString& TracksDir, const FString& Stem)
{
	for (const TCHAR* Sub : { TEXT("default"), TEXT("custom") })
	{
		const FString Path = FPaths::Combine(TracksDir, Sub, Stem, Stem + TEXT(".yaml"));
		if (IFileManager::Get().FileExists(*Path))
		{
			return Path;
		}
	}
	return FString();
}

struct FApexSimJob::FRun : public FRunnable
{
	FWork Work;
	/** Read by the library now and then. */
	volatile uint8 CancelFlag = 0;
	std::atomic<bool> bDone{ false };
	ApexPhysics::EJobResult Result = ApexPhysics::EJobResult::Failed;
	FString Error;
	double Seconds = 0.0;

	virtual uint32 Run() override
	{
		const double Started = FPlatformTime::Seconds();
		Result = Work(&CancelFlag, Error);
		Seconds = FPlatformTime::Seconds() - Started;
		bDone.store(true, std::memory_order_release);
		return 0;
	}
};

FApexSimJob::FApexSimJob() = default;

FApexSimJob::~FApexSimJob()
{
	Cancel();
}

bool FApexSimJob::Start(const FString& InLabel, FWork Work)
{
	if (Run.IsValid())
	{
		return false;
	}
	Run = MakeUnique<FRun>();
	Run->Work = MoveTemp(Work);
	// Below normal: the menu and a race keep the cores they need.
	Thread.Reset(FRunnableThread::Create(Run.Get(), TEXT("ApexSimJob"), 0, TPri_BelowNormal));
	if (!Thread.IsValid())
	{
		Run.Reset();
		return false;
	}
	Label = InLabel;
	return true;
}

bool FApexSimJob::Poll(ApexPhysics::EJobResult& OutResult, FString& OutError, double& OutSeconds)
{
	if (!Run.IsValid() || !Run->bDone.load(std::memory_order_acquire))
	{
		return false;
	}
	Thread->WaitForCompletion();
	Thread.Reset();
	OutResult = Run->Result;
	OutError = Run->Error;
	OutSeconds = Run->Seconds;
	Run.Reset();
	return true;
}

void FApexSimJob::Cancel()
{
	if (!Run.IsValid())
	{
		return;
	}
	Run->CancelFlag = 1;
	if (Thread.IsValid())
	{
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	UE_LOG(LogApexSim, Log, TEXT("%s cancelled"), *Label);
	Run.Reset();
}
