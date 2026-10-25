// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>
#include <unordered_map>

// Retail Jak 3 / Jak X builds only keep the names of a handful of symbols in memory
// (everything else points at "*unknown-symbol-name*"). Every object file on the disc still
// links its symbols by id and name, so the names are recovered from the DGO/CGO link data
// and cached per disc.
namespace Goal::SymbolNames
{
	// symbol offset from s7 -> name
	using Map = std::unordered_map<s32, std::string>;

	// names for the running disc, from the cache or, if allowed, read from the disc.
	// nullptr when unavailable
	const Map* ForDisc(bool allow_disc_read);

	// re-read the names from the disc, or from a directory of extracted DGO/CGO files
	std::string Reload(std::string_view directory);

	std::string Status();
} // namespace Goal::SymbolNames
