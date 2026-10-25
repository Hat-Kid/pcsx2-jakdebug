// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <string_view>

// A small GOAL-flavoured REPL for poking at a running Jak game. Lines starting with '('
// are evaluated as expressions, e.g. (inspect *target*) or (-> *target* control trans),
// anything else is a command (see "help").
namespace Goal::Repl
{
	// must be called on the CPU thread
	std::string Execute(std::string_view line);
} // namespace Goal::Repl
