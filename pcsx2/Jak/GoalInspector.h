// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Goal
{
	struct SExpr;
	struct FieldDef;

	// Native (no game code involved) tools for looking at objects, built on the runtime
	// model and the all-types.gc database.
	namespace Inspector
	{
		// type to use for an object: the static type if given, else the runtime type of a basic
		std::string ResolveTypeName(u32 addr, std::string_view static_type);

		// address of offset 0 of an object, field offsets in all-types are relative to it
		u32 ObjectBase(u32 addr, std::string_view type);

		std::string FormatTypedValue(const SExpr& type, u64 value, u32 addr = 0);
		u64 LoadTypedValue(const SExpr& type, u32 addr, u32 size, bool is_signed);

		std::string Dump(u32 addr, std::string_view type, u32 depth);
		std::string Guess(u32 addr, u32 size, std::string_view type);
		std::string Hexdump(u32 addr, u32 size);
		std::string Xref(u32 addr, u32 size, u32 max_results);
		std::vector<u32> FindInstances(u32 type, u32 max_results);
		std::string Instances(std::string_view type_name, u32 max_results);
		std::string FieldStats(std::string_view type_name, u32 max_instances);
		std::string TypeReport(std::string_view type_name);
		std::string Methods(std::string_view type_name);
		std::string TypeCheck(std::string_view filter);
		std::string Processes(std::string_view filter);
		std::optional<u32> FindProcess(std::string_view name);
		std::string Apropos(std::string_view needle, u32 max_results);
		std::string SymbolReport(std::string_view name);

		std::string Snapshot(const std::string& name, u32 addr, u32 size, std::string_view type);
		std::string Diff(const std::string& name);
		std::string ListSnapshots();
	} // namespace Inspector
} // namespace Goal
