#pragma once

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
