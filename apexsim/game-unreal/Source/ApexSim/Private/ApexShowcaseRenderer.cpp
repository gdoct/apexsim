#include "ApexShowcaseRenderer.h"

#include "ApexRepoPaths.h"
#include "ApexSim.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Race/ApexPhysicsLib.h"

#include <atomic>

struct FApexShowcaseRenderer::FJob : public FRunnable
{
	FInputs Inputs;
	FString Name;
	FString OutPath;
	/** Read by the library once a simulated second. */
	volatile uint8 CancelFlag = 0;
	std::atomic<bool> bDone{ false };
	bool bWritten = false;
	FString Error;
	double Seconds = 0.0;

	virtual uint32 Run() override
	{
		const double Started = FPlatformTime::Seconds();
		const ApexPhysics::EShowcaseRender Result = ApexPhysics::RenderShowcase(
			Inputs.ListPath, Inputs.TracksDir, Inputs.CarsDir, Name, OutPath, &CancelFlag, Error);
		bWritten = Result == ApexPhysics::EShowcaseRender::Written;
		Seconds = FPlatformTime::Seconds() - Started;
		bDone.store(true, std::memory_order_release);
		return 0;
	}
};

bool FApexShowcaseRenderer::FindInputs(FInputs& Out, FString& OutWhy)
{
	const bool bPackaged = FPlatformProperties::RequiresCookedData();
	FString ContentRoot;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-ApexContentDir="), ContentRoot))
	{
		// A package's server folder holds the tracks and cars it simulates:
		// <Release>/Server/content beside <Release>/Game.
		ContentRoot = bPackaged
			? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT(".."), TEXT("Server"), TEXT("content"))
			: ApexRepoPaths::ContentRoot();
	}
	ContentRoot = FPaths::ConvertRelativePathToFull(ContentRoot);
	if (!FParse::Value(FCommandLine::Get(), TEXT("-ApexShowcaseList="), Out.ListPath))
	{
		Out.ListPath = bPackaged ? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("showcase.yml"))
								 : ApexRepoPaths::CodePath(TEXT("showcase.yml"));
	}
	Out.ListPath = FPaths::ConvertRelativePathToFull(Out.ListPath);
	Out.TracksDir = FPaths::Combine(ContentRoot, TEXT("tracks"));
	// The shipped cars only, as the build's render: the player's own stay out.
	const FString Cars = FPaths::Combine(ContentRoot, TEXT("cars"));
	const FString DefaultCars = FPaths::Combine(Cars, TEXT("default"));
	Out.CarsDir = IFileManager::Get().DirectoryExists(*DefaultCars) ? DefaultCars : Cars;

	if (!IFileManager::Get().FileExists(*Out.ListPath))
	{
		OutWhy = FString::Printf(TEXT("no showcase list at %s"), *Out.ListPath);
		return false;
	}
	if (!IFileManager::Get().DirectoryExists(*Out.TracksDir) || !IFileManager::Get().DirectoryExists(*Out.CarsDir))
	{
		OutWhy = FString::Printf(TEXT("no tracks and cars under %s"), *ContentRoot);
		return false;
	}
	return true;
}

FString FApexShowcaseRenderer::CacheDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ApexSim"), TEXT("Showcases")));
}

FString FApexShowcaseRenderer::StemOf(const FString& Name)
{
	int32 Dot = INDEX_NONE;
	return Name.FindChar(TEXT('.'), Dot) ? Name.Left(Dot) : Name;
}

FApexShowcaseRenderer::FApexShowcaseRenderer(const FInputs& InInputs)
	: Inputs(InInputs)
{
}

FApexShowcaseRenderer::~FApexShowcaseRenderer()
{
	Cancel();
}

bool FApexShowcaseRenderer::Start(const FString& Name)
{
	if (Job.IsValid())
	{
		return false;
	}
	const FString Dir = CacheDirectory();
	IFileManager::Get().MakeDirectory(*Dir, true);
	Job = MakeUnique<FJob>();
	Job->Inputs = Inputs;
	Job->Name = Name;
	Job->OutPath = FPaths::Combine(Dir, Name + TEXT(".apxs"));
	Current = Name;
	// Below normal: the menu and a race keep the cores they need.
	Thread.Reset(FRunnableThread::Create(Job.Get(), TEXT("ApexShowcaseRender"), 0, TPri_BelowNormal));
	if (!Thread.IsValid())
	{
		Job.Reset();
		Current.Reset();
		return false;
	}
	UE_LOG(LogApexSim, Log, TEXT("Showcase render: %s into %s"), *Name, *Dir);
	return true;
}

bool FApexShowcaseRenderer::Poll(FString& OutName, FString& OutPath, FString& OutError)
{
	if (!Job.IsValid() || !Job->bDone.load(std::memory_order_acquire))
	{
		return false;
	}
	Thread->WaitForCompletion();
	Thread.Reset();
	OutName = Job->Name;
	OutPath = Job->bWritten ? Job->OutPath : FString();
	OutError = Job->Error;
	if (Job->bWritten)
	{
		UE_LOG(LogApexSim, Log, TEXT("Showcase render: %s done in %.1f s"), *Job->Name, Job->Seconds);
	}
	else
	{
		UE_LOG(LogApexSim, Warning, TEXT("Showcase render: %s failed: %s"), *Job->Name, *Job->Error);
	}
	Job.Reset();
	Current.Reset();
	return true;
}

void FApexShowcaseRenderer::Cancel()
{
	if (!Job.IsValid())
	{
		return;
	}
	Job->CancelFlag = 1;
	if (Thread.IsValid())
	{
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	UE_LOG(LogApexSim, Log, TEXT("Showcase render: %s cancelled"), *Job->Name);
	Job.Reset();
	Current.Reset();
}
