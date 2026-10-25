// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

// Records what GOAL functions and methods are called with and what they return, to help
// figure out argument and return types for decompilation.
//
// There are two ways to collect data:
//  - "all" mode (EmuCore/EnableJakFunctionAnalysis) hooks every `jalr ra, t9` call site
//    and the instruction the call returns to.
//  - targeted mode hooks only the entry points of chosen functions/methods (and the
//    places they return to), so the rest of the game runs at full speed.
//
// Hooks are compiled into recompiled blocks, so nothing is emitted when both are off.
// Everything here runs on the CPU thread.
namespace GoalAnalysis
{
	// compile time (recompiler) queries
	bool ShouldHookCallSites();
	bool HasHookAddresses();
	bool IsHookAddress(u32 pc);
	void NoteCallSite(u32 return_addr);

	// runtime hooks
	void OnCallSite(u32 return_addr);
	void OnHookAddress(u32 pc);

	// interpreter support, checked per instruction while hooks are active
	extern bool g_interpreter_hooks;
	void InterpreterCheck(u32 pc);

	// targeted mode, names are resolved through the symbol table / method tables
	std::string Trace(const std::vector<std::string>& targets);
	std::string Untrace(const std::vector<std::string>& targets);
	std::string ListTraced();

	void OnVSync(u32 frame);
	void Reset();
	void OnSettingsChanged(bool all_mode_enabled);

	std::string Show(std::string_view target);
	std::string Top(u32 count);
	std::string Stats();
	std::string WriteJson(std::string path, bool background);
	void SetAutoDump(u32 frames);

	std::string OverlayText();

	// read by the overlay on the gs thread
	extern std::atomic<u64> g_unique_signatures;
	extern std::atomic<u64> g_calls;
	extern std::atomic<u64> g_functions;
	extern std::atomic<bool> g_active;
} // namespace GoalAnalysis
