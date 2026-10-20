#include "ApexShowcaseRenderer.h"

#include "ApexRepoPaths.h"
#include "ApexSim.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

bool FApexShowcaseRenderer::FindInputs(FInputs& Out, FString& OutWhy)
{
	if (!FParse::Value(FCommandLine::Get(), TEXT("-ApexShowcaseList="), Out.ListPath))
	{
		Out.ListPath = FPlatformProperties::RequiresCookedData()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("showcase.yml"))
			: ApexRepoPaths::CodePath(TEXT("showcase.yml"));
	}
	Out.ListPath = FPaths::ConvertRelativePathToFull(Out.ListPath);
	if (!IFileManager::Get().FileExists(*Out.ListPath))
	{
		OutWhy = FString::Printf(TEXT("no showcase list at %s"), *Out.ListPath);
		return false;
	}
	return ApexSimContent::Find(Out.TracksDir, Out.CarsDir, OutWhy);
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

bool FApexShowcaseRenderer::Start(const FString& Name)
{
	if (Job.IsBusy())
	{
		return false;
	}
	const FString Dir = CacheDirectory();
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString OutPath = FPaths::Combine(Dir, Name + TEXT(".apxs"));
	const FInputs In = Inputs;
	const bool bStarted = Job.Start(FString::Printf(TEXT("Showcase render %s"), *Name),
		[In, Name, OutPath](const volatile uint8* Cancel, FString& OutError) {
			return ApexPhysics::RenderShowcase(In.ListPath, In.TracksDir, In.CarsDir, Name, OutPath, Cancel, OutError);
		});
	if (bStarted)
	{
		Current = Name;
		CurrentPath = OutPath;
		UE_LOG(LogApexSim, Log, TEXT("Showcase render: %s into %s"), *Name, *Dir);
	}
	return bStarted;
}

bool FApexShowcaseRenderer::Poll(FString& OutName, FString& OutPath, FString& OutError)
{
	ApexPhysics::EJobResult Result;
	double Seconds = 0.0;
	if (!Job.Poll(Result, OutError, Seconds))
	{
		return false;
	}
	OutName = Current;
	OutPath = Result == ApexPhysics::EJobResult::Done ? CurrentPath : FString();
	if (OutPath.IsEmpty())
	{
		UE_LOG(LogApexSim, Warning, TEXT("Showcase render: %s failed: %s"), *Current, *OutError);
	}
	else
	{
		UE_LOG(LogApexSim, Log, TEXT("Showcase render: %s done in %.1f s"), *Current, Seconds);
	}
	Current.Reset();
	CurrentPath.Reset();
	return true;
}

void FApexShowcaseRenderer::Cancel()
{
	Job.Cancel();
	Current.Reset();
	CurrentPath.Reset();
}
