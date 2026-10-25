// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalRuntime.h"
#include "GoalTypeDb.h"

#include "Memory.h"
#include "R5900.h"
#include "VMManager.h"
#include "vtlb.h"

#include "common/StringUtil.h"

#include "fmt/format.h"

#include <bit>
#include <cstring>

namespace Goal
{
	static constexpr u32 SCRATCHPAD_BASE = 0x70000000;
	static constexpr u32 MAX_TYPE_DEPTH = 64;

	// see common/symbols.h and kscheme.cpp InitHeapAndSymbol() in jak-project
	static constexpr Layout s_layouts[] = {
		{GameVersion::Jak1, 8, 0, 0x8004, 0x10000, 4, 0x8, -0xa, 0x10, 0x20, 0x28, 0x30, 0xe0, -1, 0xff38},
		{GameVersion::Jak2, 4, -1, 0x8001, 0x10000, 5, 0x4, -0x7, 0x8, 0x10, 0x14, 0x18, 0x70, -1, 0xff37},
		{GameVersion::Jak3, 4, -1, 0x8001, 0x10000, 5, 0x4, -0x7, 0x8, 0x10, 0x14, 0x18, 0x70, 0x28c, 0},
		{GameVersion::JakX, 4, -1, 0x8001, 0x10000, 5, 0x4, -0x7, 0x8, 0x10, 0x14, 0x18, 0x70, 0x28c, 0},
	};

	const Layout& GetLayout(GameVersion version)
	{
		return s_layouts[static_cast<int>(version)];
	}

	const char* GameVersionName(GameVersion version)
	{
		switch (version)
		{
			case GameVersion::Jak1:
				return "jak1";
			case GameVersion::Jak2:
				return "jak2";
			case GameVersion::Jak3:
				return "jak3";
			case GameVersion::JakX:
			default:
				return "jakx";
		}
	}

	std::optional<GameVersion> ParseGameVersion(std::string_view name)
	{
		if (name == "jak1")
			return GameVersion::Jak1;
		if (name == "jak2")
			return GameVersion::Jak2;
		if (name == "jak3")
			return GameVersion::Jak3;
		if (name == "jakx")
			return GameVersion::JakX;
		return std::nullopt;
	}

	std::optional<GameVersion> GameVersionFromSerial(std::string_view serial)
	{
		static constexpr std::pair<const char*, GameVersion> serials[] = {
			{"SCUS-97124", GameVersion::Jak1},
			{"SCES-50361", GameVersion::Jak1},
			{"SCPS-15021", GameVersion::Jak1},
			{"SCUS-97265", GameVersion::Jak2},
			{"SCUS-97273", GameVersion::Jak2},
			{"SCES-51608", GameVersion::Jak2},
			{"SCPS-15057", GameVersion::Jak2},
			{"SCUS-97330", GameVersion::Jak3},
			{"SCES-52460", GameVersion::Jak3},
			{"SCUS-97429", GameVersion::JakX},
			{"SCES-53286", GameVersion::JakX},
		};
		for (const auto& [s, v] : serials)
		{
			if (serial == s)
				return v;
		}
		return std::nullopt;
	}

	//////////////////////////////////////////////////////////////////////////
	// Memory
	//////////////////////////////////////////////////////////////////////////

	u32 Mem::RamSize()
	{
		return Ps2MemSize::ExposedRam;
	}

	std::span<const u8> Mem::Ram()
	{
		return std::span<const u8>(eeMem->Main, Ps2MemSize::ExposedRam);
	}

	bool Mem::IsRam(u32 addr)
	{
		return addr < Ps2MemSize::ExposedRam;
	}

	bool Mem::IsScratchpad(u32 addr)
	{
		return addr >= SCRATCHPAD_BASE && addr < SCRATCHPAD_BASE + Ps2MemSize::Scratch;
	}

	static const u8* DirectPtr(u32 addr, u32 size)
	{
		if (addr < Ps2MemSize::ExposedRam && size <= Ps2MemSize::ExposedRam - addr)
			return &eeMem->Main[addr];
		if (Mem::IsScratchpad(addr) && size <= SCRATCHPAD_BASE + Ps2MemSize::Scratch - addr)
			return &eeMem->Scratch[addr - SCRATCHPAD_BASE];
		return nullptr;
	}

	bool Mem::Valid(u32 addr, u32 size)
	{
		return DirectPtr(addr, size) != nullptr;
	}

	bool Mem::Read(u32 addr, void* dst, u32 size)
	{
		if (const u8* ptr = DirectPtr(addr, size))
		{
			std::memcpy(dst, ptr, size);
			return true;
		}
		std::memset(dst, 0, size);
		return false;
	}

	bool Mem::Write(u32 addr, const void* src, u32 size)
	{
		if (!Valid(addr, size))
			return false;

		// goes through the vtlb so recompiled code in the written pages gets invalidated
		return vtlb_memSafeWriteBytes(addr, src, size);
	}

	u8 Mem::Read8(u32 addr)
	{
		const u8* ptr = DirectPtr(addr, 1);
		return ptr ? *ptr : 0;
	}

	u16 Mem::Read16(u32 addr)
	{
		u16 value;
		Read(addr, &value, sizeof(value));
		return value;
	}

	u32 Mem::Read32(u32 addr)
	{
		u32 value;
		Read(addr, &value, sizeof(value));
		return value;
	}

	u64 Mem::Read64(u32 addr)
	{
		u64 value;
		Read(addr, &value, sizeof(value));
		return value;
	}

	float Mem::ReadFloat(u32 addr)
	{
		return std::bit_cast<float>(Read32(addr));
	}

	void Mem::Write32(u32 addr, u32 value)
	{
		Write(addr, &value, sizeof(value));
	}

	std::string Mem::ReadCString(u32 addr, u32 max_len)
	{
		std::string result;
		for (u32 i = 0; i < max_len; i++)
		{
			const u8* ptr = DirectPtr(addr + i, 1);
			if (!ptr || *ptr == 0)
				break;
			result.push_back(static_cast<char>(*ptr));
		}
		return result;
	}

	bool Mem::IsPrintableCString(u32 addr, u32 min_len, u32 max_len)
	{
		for (u32 i = 0; i < max_len; i++)
		{
			const u8* ptr = DirectPtr(addr + i, 1);
			if (!ptr)
				return false;
			if (*ptr == 0)
				return i >= min_len;
			if ((*ptr < 0x20 || *ptr > 0x7e) && *ptr != '\n' && *ptr != '\t')
				return false;
		}
		return false;
	}

	//////////////////////////////////////////////////////////////////////////
	// Runtime
	//////////////////////////////////////////////////////////////////////////

	Runtime& GetRuntime()
	{
		static Runtime s_runtime;
		return s_runtime;
	}

	Runtime::Runtime()
		: m_layout(&Goal::GetLayout(GameVersion::JakX))
	{
	}

	void Runtime::SetVersionOverride(std::optional<GameVersion> version)
	{
		m_version_override = version;
		m_valid = false;
		m_s7 = 0;
		m_symbol_cache.clear();
		m_code_map.clear();
	}

	bool Runtime::ValidateS7(u32 s7) const
	{
		const Layout& l = *m_layout;
		if (!Mem::Valid(s7 - l.table_below_s7, l.table_size))
			return false;

		const u32 false_value = Mem::Read32(s7 + l.symbol_value_off);
		const u32 true_value = Mem::Read32(s7 + l.fix_true + l.symbol_value_off);
		if (false_value != s7 || true_value != s7 + static_cast<u32>(l.fix_true))
			return false;

		// the type of type is type
		const u32 type_type = Mem::Read32(s7 + l.fix_type_type + l.symbol_value_off);
		return Mem::Valid(type_type - 4, 4) && Mem::Read32(type_type - 4) == type_type;
	}

	std::optional<u32> Runtime::ScanForS7() const
	{
		const Layout& l = *m_layout;
		const std::span<const u8> ram = Mem::Ram();
		// tables are at least 16 byte aligned, so s7 is too (modulo its own offset)
		const u32 s7_align = l.table_below_s7 & 0xf;
		const u32 value_delta = static_cast<u32>(l.symbol_value_off);
		for (u32 addr = 0x100000; addr + 16 < ram.size(); addr += 16)
		{
			const u32 s7 = addr + s7_align;
			const u32 value_addr = s7 + value_delta;
			if (value_addr + 4 > ram.size())
				break;
			u32 word;
			std::memcpy(&word, &ram[value_addr], 4);
			if (word != s7)
				continue;
			if (ValidateS7(s7))
				return s7;
		}
		return std::nullopt;
	}

	void Runtime::ApplyS7(u32 s7)
	{
		const Layout& l = *m_layout;
		if (s7 != m_s7)
		{
			m_symbol_cache.clear();
			m_code_map.clear();
			m_unknown_name = 0;
			m_recovered_names = nullptr;
		}
		m_s7 = s7;
		m_table_start = s7 - l.table_below_s7;
		m_symbol_string = (l.fix_symbol_string >= 0) ? SymbolValue(s7 + l.fix_symbol_string) : 0;
		m_type_type = SymbolValue(s7 + l.fix_type_type);
		m_function_type = SymbolValue(s7 + l.fix_function_type);
		m_string_type = SymbolValue(s7 + l.fix_string_type);
		m_symbol_type = SymbolValue(s7 + l.fix_symbol_type);
		m_pair_type = SymbolValue(s7 + l.fix_pair_type);
	}

	bool Runtime::Refresh(bool allow_scan)
	{
		if (!VMManager::HasValidVM())
		{
			m_valid = false;
			m_error = "no game running";
			return false;
		}

		GameVersion version = GameVersion::JakX;
		if (m_version_override.has_value())
			version = *m_version_override;
		else if (const auto detected = GameVersionFromSerial(VMManager::GetDiscSerial()))
			version = *detected;

		if (version != m_version || m_layout != &Goal::GetLayout(version))
		{
			m_version = version;
			m_layout = &Goal::GetLayout(version);
			m_s7 = 0;
			m_symbol_cache.clear();
			m_code_map.clear();
		}

		const u32 reg_s7 = cpuRegs.GPR.n.s7.UL[0];
		if (ValidateS7(reg_s7))
		{
			ApplyS7(reg_s7);
		}
		else if (m_s7 != 0 && ValidateS7(m_s7))
		{
			ApplyS7(m_s7);
		}
		else if (allow_scan)
		{
			const std::optional<u32> found = ScanForS7();
			if (!found.has_value())
			{
				m_valid = false;
				m_error = fmt::format("could not find the {} symbol table (has the game kernel booted?)", GameVersionName(m_version));
				return false;
			}
			ApplyS7(*found);
		}
		else
		{
			m_valid = false;
			m_error = "s7 does not point at a symbol table";
			return false;
		}

		m_valid = true;
		m_error.clear();
		UpdateRecoveredNames(allow_scan);
		return true;
	}

	u32 Runtime::FindUnknownNameString() const
	{
		static constexpr char UNKNOWN_NAME[] = "*unknown-symbol-name*";
		const Layout& l = *m_layout;
		for (u32 sym = m_table_start + l.first_symbol_off; sym < m_table_start + l.table_size; sym += l.symbol_size)
		{
			const u32 str = Mem::Read32(m_symbol_string + (sym - m_s7));
			char text[sizeof(UNKNOWN_NAME)];
			if (str != 0 && (str & 7) == 4 && Mem::Read(str + 4, text, sizeof(text)) && std::memcmp(text, UNKNOWN_NAME, sizeof(text)) == 0)
				return str;
		}
		return 0;
	}

	void Runtime::UpdateRecoveredNames(bool allow_disc_read)
	{
		if (m_layout->fix_symbol_string < 0 || m_symbol_string == 0)
			return;
		if (m_unknown_name == 0)
			m_unknown_name = FindUnknownNameString();
		if (m_unknown_name == 0)
			return;

		const SymbolNames::Map* names = SymbolNames::ForDisc(allow_disc_read);
		if (names != m_recovered_names)
		{
			m_recovered_names = names;
			m_symbol_cache.clear();
			m_code_map.clear();
		}
	}

	bool Runtime::IsSymbol(u32 addr) const
	{
		const Layout& l = *m_layout;
		const u32 first = m_table_start + l.first_symbol_off;
		if (addr < first || addr >= m_table_start + l.table_size)
			return false;
		return ((addr - first) % l.symbol_size) == 0;
	}

	u32 Runtime::SymbolValue(u32 sym) const
	{
		return Mem::Read32(SymbolValueAddr(sym));
	}

	std::string Runtime::SymbolName(u32 sym) const
	{
		const Layout& l = *m_layout;
		u32 str;
		if (l.fix_symbol_string >= 0)
		{
			if (m_symbol_string == 0)
				return {};
			str = Mem::Read32(m_symbol_string + (sym - m_s7));
			if (str == m_unknown_name && str != 0)
			{
				if (!m_recovered_names)
					return {};
				const auto it = m_recovered_names->find(static_cast<s32>(sym - m_s7));
				return it != m_recovered_names->end() ? it->second : std::string();
			}
		}
		else
		{
			str = Mem::Read32(sym + l.name_offset);
		}

		if (str == 0 || (str & 7) != 4 || !Mem::Valid(str, 8))
			return {};

		std::string name = Mem::ReadCString(str + 4, 256);
		for (const char c : name)
		{
			if (c <= ' ' || c > '~')
				return {};
		}
		return name;
	}

	void Runtime::ForEachSymbol(const std::function<void(u32 sym, const std::string& name)>& fn)
	{
		const Layout& l = *m_layout;
		for (u32 sym = m_table_start + l.first_symbol_off; sym < m_table_start + l.table_size; sym += l.symbol_size)
		{
			std::string name = SymbolName(sym);
			if (!name.empty())
				fn(sym, name);
		}
	}

	std::optional<u32> Runtime::FindSymbol(std::string_view name)
	{
		if (name == "#f")
			return m_s7;
		if (name == "#t")
			return TrueSym();

		auto it = m_symbol_cache.find(std::string(name));
		if (it != m_symbol_cache.end() && SymbolName(it->second) == name)
			return it->second;

		m_symbol_cache.clear();
		ForEachSymbol([this](u32 sym, const std::string& sym_name) { m_symbol_cache.emplace(sym_name, sym); });
		it = m_symbol_cache.find(std::string(name));
		if (it != m_symbol_cache.end())
			return it->second;
		return std::nullopt;
	}

	bool Runtime::IsType(u32 addr) const
	{
		return addr != 0 && (addr & 7) == 4 && Mem::Valid(addr - 4, 0x20) && Mem::Read32(addr - 4) == m_type_type;
	}

	std::optional<u32> Runtime::FindType(std::string_view name)
	{
		const std::optional<u32> sym = FindSymbol(name);
		if (!sym.has_value())
			return std::nullopt;
		const u32 value = SymbolValue(*sym);
		if (!IsType(value))
			return std::nullopt;
		return value;
	}

	std::string Runtime::TypeName(u32 type) const
	{
		if (!IsType(type))
			return {};
		return SymbolName(Mem::Read32(type));
	}

	u32 Runtime::TypeParent(u32 type) const
	{
		return Mem::Read32(type + 4);
	}

	u16 Runtime::TypeSize(u32 type) const
	{
		return Mem::Read16(type + 8);
	}

	u16 Runtime::TypeHeapBase(u32 type) const
	{
		return Mem::Read16(type + 0xc);
	}

	u16 Runtime::TypeMethodCount(u32 type) const
	{
		return Mem::Read16(type + 0xe);
	}

	u32 Runtime::TypeMethod(u32 type, u32 id) const
	{
		return Mem::Read32(type + 16 + id * 4);
	}

	std::vector<u32> Runtime::TypeChain(u32 type) const
	{
		std::vector<u32> chain;
		for (u32 i = 0; i < MAX_TYPE_DEPTH && IsType(type); i++)
		{
			chain.push_back(type);
			const u32 parent = TypeParent(type);
			if (parent == type)
				break;
			type = parent;
		}
		return chain;
	}

	bool Runtime::TypeInherits(u32 type, u32 ancestor) const
	{
		for (const u32 t : TypeChain(type))
		{
			if (t == ancestor)
				return true;
		}
		return false;
	}

	bool Runtime::TypeInherits(u32 type, std::string_view ancestor_name) const
	{
		for (const u32 t : TypeChain(type))
		{
			if (TypeName(t) == ancestor_name)
				return true;
		}
		return false;
	}

	bool Runtime::IsBasic(u32 obj) const
	{
		return BasicType(obj) != 0;
	}

	u32 Runtime::BasicType(u32 obj) const
	{
		if ((obj & 7) != 4 || !Mem::Valid(obj - 4, 4))
			return 0;
		const u32 type = Mem::Read32(obj - 4);
		return IsType(type) ? type : 0;
	}

	bool Runtime::IsFunction(u32 obj) const
	{
		return m_function_type != 0 && (obj & 7) == 4 && Mem::Valid(obj - 4, 8) && Mem::Read32(obj - 4) == m_function_type;
	}

	bool Runtime::IsString(u32 obj) const
	{
		return m_string_type != 0 && (obj & 7) == 4 && Mem::Valid(obj - 4, 8) && Mem::Read32(obj - 4) == m_string_type;
	}

	std::string Runtime::StringValue(u32 str, u32 max_len) const
	{
		const u32 allocated = Mem::Read32(str);
		return Mem::ReadCString(str + 4, std::min(max_len, allocated + 1));
	}

	u32 Runtime::DynamicTypeOf(u64 value) const
	{
		const ValueInfo info = Classify(value);
		switch (info.kind)
		{
			case ValueKind::False:
			case ValueKind::True:
			case ValueKind::Symbol:
				return m_symbol_type;
			case ValueKind::Basic:
			case ValueKind::Type:
			case ValueKind::Function:
			case ValueKind::String:
				return BasicType(static_cast<u32>(value));
			case ValueKind::Pair:
			case ValueKind::EmptyPair:
				return m_pair_type;
			default:
				return 0;
		}
	}

	ValueInfo Runtime::Classify(u64 value) const
	{
		if (value == 0)
			return {ValueKind::Zero, 0};

		const u32 lo = static_cast<u32>(value);
		const u32 hi = static_cast<u32>(value >> 32);
		const bool is_32bit = (hi == 0 && !(lo & 0x80000000u)) || (hi == 0xffffffffu && (lo & 0x80000000u)) || (hi == 0 && (lo & 0x80000000u));
		if (!is_32bit)
			return {ValueKind::BigInt, 0};

		if (m_valid)
		{
			if (lo == m_s7)
				return {ValueKind::False, lo};
			if (lo == TrueSym())
				return {ValueKind::True, lo};
			if (lo == EmptyPair())
				return {ValueKind::EmptyPair, lo};
			if (IsSymbol(lo))
				return {ValueKind::Symbol, lo};

			if ((lo & 7) == 4)
			{
				if (const u32 type = BasicType(lo))
				{
					if (type == m_type_type)
						return {ValueKind::Type, type};
					if (type == m_function_type)
						return {ValueKind::Function, type};
					if (type == m_string_type)
						return {ValueKind::String, type};
					return {ValueKind::Basic, type};
				}
			}

			if ((lo & 7) == 2 && lo >= 0x100000 && Mem::IsRam(lo) && Mem::Valid(lo - 2, 8))
			{
				const u32 car = Mem::Read32(lo - 2);
				const u32 cdr = Mem::Read32(lo + 2);
				const auto plausible = [this](u32 v) { return v == 0 || Mem::IsRam(v) || (v & 7) == 0 || v == EmptyPair(); };
				if (plausible(car) && plausible(cdr) && ((cdr & 7) == 2 || cdr == EmptyPair()))
					return {ValueKind::Pair, lo};
			}
		}

		if (lo >= 0x100000 && Mem::IsRam(lo))
			return {ValueKind::Pointer, lo};
		if (Mem::IsScratchpad(lo))
			return {ValueKind::ScratchPointer, lo};

		const u32 exponent = (lo >> 23) & 0xff;
		if (exponent >= 0x60 && exponent <= 0x9f)
			return {ValueKind::Float, lo};

		const s32 as_int = static_cast<s32>(lo);
		if (as_int >= -0x100000 && as_int <= 0x100000)
			return {ValueKind::Int, lo};

		return {ValueKind::Unknown, lo};
	}

	std::string Runtime::HeapName(u32 addr) const
	{
		if (!m_valid)
			return {};
		if (Mem::IsScratchpad(addr))
			return "scratchpad";
		if (addr >= m_table_start && addr < m_table_start + m_layout->table_size)
			return "symbol-table";

		Runtime& self = const_cast<Runtime&>(*this);
		for (const char* heap : {"debug", "global"})
		{
			const std::optional<u32> sym = self.FindSymbol(heap);
			if (!sym.has_value())
				continue;
			const u32 kheap = SymbolValue(*sym);
			if (!Mem::Valid(kheap, 16))
				continue;
			const u32 base = Mem::Read32(kheap);
			const u32 top_base = Mem::Read32(kheap + 12);
			if (addr >= base && addr < top_base)
				return heap;
		}
		return {};
	}

	std::string Runtime::DescribeAddress(u32 addr) const
	{
		if (IsSymbol(addr))
			return fmt::format("symbol '{}", SymbolName(addr));
		if (const auto code = LookupContainingCode(addr); code.has_value() && addr - code->first < 0x10000)
		{
			if (addr == code->first)
				return CodeLabelName(*code->second);
			return fmt::format("{}+{:#x}", CodeLabelName(*code->second), addr - code->first);
		}
		const std::string heap = HeapName(addr);
		return heap.empty() ? std::string() : fmt::format("in {} heap", heap);
	}

	std::string Runtime::Describe(u64 value, bool verbose) const
	{
		return DescribeInfo(Classify(value), value, verbose);
	}

	std::string Runtime::DescribeInfo(const ValueInfo& info, u64 value, bool verbose) const
	{
		const u32 lo = static_cast<u32>(value);
		switch (info.kind)
		{
			case ValueKind::Zero:
				return "0";
			case ValueKind::False:
				return "#f";
			case ValueKind::True:
				return "#t";
			case ValueKind::EmptyPair:
				return "'()";
			case ValueKind::Symbol:
				return fmt::format("'{}", SymbolName(lo));
			case ValueKind::Type:
				return fmt::format("<type {}>", TypeName(lo));
			case ValueKind::Function:
			{
				if (const CodeLabel* label = LookupCode(lo))
					return fmt::format("<function {} @ #x{:x}>", CodeLabelName(*label), lo);
				return fmt::format("<function @ #x{:x}>", lo);
			}
			case ValueKind::String:
			{
				std::string str = StringValue(lo, 80);
				StringUtil::ReplaceAll(&str, "\n", "\\n");
				return fmt::format("\"{}\"", str);
			}
			case ValueKind::Basic:
			{
				std::string desc = fmt::format("#<{} @ #x{:x}>", TypeName(info.detail), lo);
				if (TypeInherits(info.detail, "process-tree"))
				{
					const u32 name = Mem::Read32(lo);
					if (IsString(name))
						desc = fmt::format("#<{} \"{}\" @ #x{:x}>", TypeName(info.detail), StringValue(name, 64), lo);
				}
				return desc;
			}
			case ValueKind::Pair:
				return fmt::format("(pair @ #x{:x})", lo);
			case ValueKind::Pointer:
			case ValueKind::ScratchPointer:
			{
				if (!verbose)
					return fmt::format("#x{:x}", lo);
				const std::string where = DescribeAddress(lo);
				return where.empty() ? fmt::format("#x{:x}", lo) : fmt::format("#x{:x} ({})", lo, where);
			}
			case ValueKind::Float:
			{
				const float f = std::bit_cast<float>(lo);
				return verbose ? fmt::format("{}f (#x{:x})", f, lo) : fmt::format("{}f", f);
			}
			case ValueKind::Int:
				return verbose ? fmt::format("{} (#x{:x})", static_cast<s32>(lo), lo) : fmt::format("{}", static_cast<s32>(lo));
			case ValueKind::BigInt:
				return fmt::format("#x{:x}", value);
			case ValueKind::Unknown:
			default:
				return fmt::format("#x{:x}", lo);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// Code map
	//////////////////////////////////////////////////////////////////////////

	void Runtime::RebuildCodeMap()
	{
		m_code_map.clear();
		if (!m_valid)
			return;

		// state handler slots come from all-types, if it is loaded
		const TypeDb& db = GetTypeDb();
		std::vector<std::pair<std::string, u32>> state_slots;
		u32 state_name_offset = 0;
		if (const FieldDef* name_field = db.FindField("state", "name"))
		{
			state_name_offset = static_cast<u32>(name_field->offset);
			for (const char* slot : {"code", "trans", "post", "enter", "exit", "event"})
			{
				if (const FieldDef* f = db.FindField("state", slot))
					state_slots.emplace_back(slot, static_cast<u32>(f->offset));
			}
		}
		const std::optional<u32> state_type = FindType("state");

		const auto add_state = [&](u32 state, const std::string& fallback_name) {
			if (state_slots.empty())
				return;
			std::string name = fallback_name;
			const u32 name_sym = Mem::Read32(state - 4 + state_name_offset);
			if (IsSymbol(name_sym))
				name = SymbolName(name_sym);
			for (const auto& [slot, offset] : state_slots)
			{
				const u32 fn = Mem::Read32(state - 4 + offset);
				if (IsFunction(fn) && !m_code_map.contains(fn))
					m_code_map.emplace(fn, CodeLabel{CodeLabel::Kind::State, name, -1, slot});
			}
		};

		std::vector<std::pair<u32, std::string>> types;
		std::vector<std::pair<u32, std::string>> states;
		ForEachSymbol([&](u32 sym, const std::string& name) {
			const u32 value = SymbolValue(sym);
			if (IsFunction(value))
			{
				m_code_map.try_emplace(value, CodeLabel{CodeLabel::Kind::Function, name});
			}
			else if (IsType(value))
			{
				types.emplace_back(value, name);
			}
			else if (state_type.has_value() && BasicType(value) == *state_type)
			{
				states.emplace_back(value, name);
			}
		});

		for (const auto& [type, name] : types)
		{
			const u32 count = std::min<u32>(TypeMethodCount(type), 512);
			for (u32 i = 0; i < count; i++)
			{
				const u32 method = TypeMethod(type, i);
				if (state_type.has_value() && BasicType(method) == *state_type)
				{
					states.emplace_back(method, fmt::format("{}-state-{}", name, i));
					continue;
				}
				if (!IsFunction(method) || m_code_map.contains(method))
					continue;

				// attribute the method to the type that introduced this implementation
				u32 owner = type;
				for (u32 depth = 0; depth < MAX_TYPE_DEPTH; depth++)
				{
					const u32 parent = TypeParent(owner);
					if (!IsType(parent) || parent == owner || TypeMethodCount(parent) <= i || TypeMethod(parent, i) != method)
						break;
					owner = parent;
				}
				m_code_map.emplace(method, CodeLabel{CodeLabel::Kind::Method, TypeName(owner), static_cast<s32>(i)});
			}
		}

		for (const auto& [state, name] : states)
			add_state(state, name);
	}

	const CodeLabel* Runtime::LookupCode(u32 addr) const
	{
		const auto it = m_code_map.find(addr);
		return it != m_code_map.end() ? &it->second : nullptr;
	}

	std::optional<std::pair<u32, const CodeLabel*>> Runtime::LookupContainingCode(u32 addr) const
	{
		auto it = m_code_map.upper_bound(addr);
		if (it == m_code_map.begin())
			return std::nullopt;
		--it;
		return std::make_pair(it->first, &it->second);
	}

	std::string Runtime::CodeLabelName(const CodeLabel& label) const
	{
		switch (label.kind)
		{
			case CodeLabel::Kind::Method:
				return fmt::format("(method {} {})", label.method_id, label.name);
			case CodeLabel::Kind::State:
				return fmt::format("({} {})", label.state_slot, label.name);
			case CodeLabel::Kind::Function:
			default:
				return label.name;
		}
	}
} // namespace Goal
