#pragma once

#include "ApexRepoPaths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"

/**
 * Flags every ApexSim automation test registers with: any application
 * context, under the Engine filter.
 *
 * One definition, shared, because a unity build compiles every file in this
 * folder as a single translation unit — a per-file copy in an anonymous
 * namespace is a redefinition the moment two test files land in the same
 * chunk.
 */
inline constexpr EAutomationTestFlags ApexTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

/**
 * Runs the server's `apexsim-replay` (the release build in
 * server/server/target) with Args from the code root: the reference the
 * game's own showcase and guide renders are compared with. False, with why,
 * when the tool is not built or fails.
 */
inline bool ApexRunReplayTool(const FString& Args, FString& OutWhy)
{
	const FString Exe = ApexRepoPaths::CodePath(TEXT("server/server/target/release/apexsim-replay.exe"));
	if (!IFileManager::Get().FileExists(*Exe))
	{
		OutWhy = FString::Printf(TEXT("no %s (cargo build --release in server/server)"), *Exe);
		return false;
	}
	int32 Code = -1;
	FString StdOut, StdErr;
	const FString Root = ApexRepoPaths::CodePath(TEXT(""));
	if (!FPlatformProcess::ExecProcess(*Exe, *Args, &Code, &StdOut, &StdErr, *Root) || Code != 0)
	{
		OutWhy = FString::Printf(TEXT("apexsim-replay %s exited %d: %s"), *Args, Code, *StdErr.Right(400));
		return false;
	}
	return true;
}
