// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Line based TCP server for the GOAL REPL, bound to 127.0.0.1. Each request is one line,
// each response is the REPL output followed by a NUL byte. See tools/jakdebug/goal_repl.py.
namespace GoalReplServer
{
	static constexpr u16 DEFAULT_PORT = 28012;

	bool Start(u16 port);
	void Stop();
	bool IsRunning();

	// starts or stops the server to match EmuConfig.EnableJakReplServer
	void ApplySettings();
} // namespace GoalReplServer
