// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GoalSymbolNames.h"

#include "common/Pcsx2Defs.h"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Model of the GOAL runtime (Jak 1/2/3/X) living in EE memory. Everything here
// only reads guest memory and must be used from the CPU thread.
namespace Goal
{
	enum class GameVersion
	{
		Jak1,
		Jak2,
		Jak3,
		JakX,
	};

	const char* GameVersionName(GameVersion version);
	std::optional<GameVersion> ParseGameVersion(std::string_view name);
	std::optional<GameVersion> GameVersionFromSerial(std::string_view serial);

	namespace Mem
	{
		// main ram (32mb or 128mb) and the scratchpad are considered valid
		bool Valid(u32 addr, u32 size = 1);
		bool IsRam(u32 addr);
		bool IsScratchpad(u32 addr);
		u32 RamSize();
		std::span<const u8> Ram();

		bool Read(u32 addr, void* dst, u32 size);
		bool Write(u32 addr, const void* src, u32 size);

		u8 Read8(u32 addr);
		u16 Read16(u32 addr);
		u32 Read32(u32 addr);
		u64 Read64(u32 addr);
		float ReadFloat(u32 addr);
		void Write32(u32 addr, u32 value);

		std::string ReadCString(u32 addr, u32 max_len = 4096);
		bool IsPrintableCString(u32 addr, u32 min_len, u32 max_len);
	} // namespace Mem

	// fixed symbol offsets relative to s7 that differ between games
	struct Layout
	{
		GameVersion version;
		u32 symbol_size;      // 8 in jak 1, 4 afterwards
		s32 symbol_value_off; // where the value lives relative to the symbol address
		u32 table_below_s7;   // s7 - table start
		u32 table_size;
		u32 first_symbol_off; // first symbol relative to the table start
		s32 fix_true;
		s32 fix_empty_pair;
		s32 fix_function_type;
		s32 fix_string_type;
		s32 fix_symbol_type;
		s32 fix_type_type;
		s32 fix_pair_type;
		s32 fix_symbol_string; // -1 when the game uses a fixed name offset instead
		u32 name_offset;       // used when fix_symbol_string < 0
	};

	const Layout& GetLayout(GameVersion version);

	enum class ValueKind : u8
	{
		Zero,
		False,
		True,
		Symbol,
		Basic,
		Type,
		Function,
		String,
		Pair,
		EmptyPair,
		Pointer,
		ScratchPointer,
		Float,
		Int,
		BigInt,
		Unknown,
	};

	struct ValueInfo
	{
		ValueKind kind = ValueKind::Unknown;
		u32 detail = 0; // symbol address, type address, pointed-to address...
	};

	struct CodeLabel
	{
		enum class Kind : u8
		{
			Function,
			Method,
			State,
		};

		Kind kind = Kind::Function;
		std::string name; // function / state name, or type name for methods
		s32 method_id = -1;
		std::string state_slot; // code, trans, enter...
	};

	class Runtime
	{
	public:
		Runtime();

		// re-reads s7 (from the register or a memory scan) and validates the layout
		bool Refresh(bool allow_scan = true);
		bool IsValid() const { return m_valid; }
		const std::string& Error() const { return m_error; }

		GameVersion Version() const { return m_version; }
		void SetVersionOverride(std::optional<GameVersion> version);
		std::optional<GameVersion> VersionOverride() const { return m_version_override; }
		const Layout& GetLayout() const { return *m_layout; }

		u32 S7() const { return m_s7; }
		u32 FalseSym() const { return m_s7; }
		u32 EmptyPair() const { return m_s7 + m_layout->fix_empty_pair; }
		u32 TrueSym() const { return m_s7 + m_layout->fix_true; }
		u32 FixedSymbolValue(s32 offset) const { return SymbolValue(m_s7 + offset); }

		u32 TypeType() const { return m_type_type; }
		u32 FunctionType() const { return m_function_type; }
		u32 StringType() const { return m_string_type; }
		u32 SymbolType() const { return m_symbol_type; }
		u32 PairType() const { return m_pair_type; }

		// symbols
		u32 SymbolTableStart() const { return m_table_start; }
		u32 SymbolTableEnd() const { return m_table_start + m_layout->table_size; }
		bool IsSymbol(u32 addr) const;
		u32 SymbolValueAddr(u32 sym) const { return sym + m_layout->symbol_value_off; }
		u32 SymbolValue(u32 sym) const;
		std::string SymbolName(u32 sym) const;
		std::optional<u32> FindSymbol(std::string_view name);
		void ForEachSymbol(const std::function<void(u32 sym, const std::string& name)>& fn);
		void InvalidateSymbolCache() { m_symbol_cache.clear(); }
		// true when most names come from the disc instead of memory (retail jak 3 / jak x)
		bool UsesRecoveredNames() const { return m_recovered_names != nullptr; }

		// types
		bool IsType(u32 addr) const;
		std::optional<u32> FindType(std::string_view name);
		std::string TypeName(u32 type) const;
		u32 TypeParent(u32 type) const;
		u16 TypeSize(u32 type) const;
		u16 TypeHeapBase(u32 type) const;
		u16 TypeMethodCount(u32 type) const;
		u32 TypeMethod(u32 type, u32 id) const;
		bool TypeInherits(u32 type, u32 ancestor) const;
		bool TypeInherits(u32 type, std::string_view ancestor_name) const;
		std::vector<u32> TypeChain(u32 type) const;

		// objects
		bool IsBasic(u32 obj) const;
		u32 BasicType(u32 obj) const; // 0 if obj is not a basic
		bool IsFunction(u32 obj) const;
		bool IsString(u32 obj) const;
		std::string StringValue(u32 str, u32 max_len = 1024) const;
		u32 DynamicTypeOf(u64 value) const; // runtime type, like rtype-of, 0 if unknown

		ValueInfo Classify(u64 value) const;
		ValueInfo ClassifyWord(u32 word) const { return Classify(static_cast<u64>(static_cast<s64>(static_cast<s32>(word)))); }
		std::string Describe(u64 value, bool verbose = false) const;
		std::string DescribeInfo(const ValueInfo& info, u64 value, bool verbose = false) const;
		std::string DescribeAddress(u32 addr) const; // "fn+0x10", "symbol foo", ...

		// heaps, for pointer descriptions. empty name if unknown
		std::string HeapName(u32 addr) const;

		// code map: function / method / state handler addresses
		void RebuildCodeMap();
		bool CodeMapEmpty() const { return m_code_map.empty(); }
		const CodeLabel* LookupCode(u32 addr) const;
		std::optional<std::pair<u32, const CodeLabel*>> LookupContainingCode(u32 addr) const;
		std::string CodeLabelName(const CodeLabel& label) const;
		size_t CodeMapSize() const { return m_code_map.size(); }

	private:
		bool ValidateS7(u32 s7) const;
		std::optional<u32> ScanForS7() const;
		void ApplyS7(u32 s7);
		u32 FindUnknownNameString() const;
		void UpdateRecoveredNames(bool allow_disc_read);

		std::optional<GameVersion> m_version_override;
		GameVersion m_version = GameVersion::JakX;
		const Layout* m_layout;

		bool m_valid = false;
		std::string m_error;
		u32 m_s7 = 0;
		u32 m_table_start = 0;
		u32 m_symbol_string = 0;
		u32 m_unknown_name = 0; // the string retail builds give symbols without a stored name
		const SymbolNames::Map* m_recovered_names = nullptr;
		u32 m_type_type = 0;
		u32 m_function_type = 0;
		u32 m_string_type = 0;
		u32 m_symbol_type = 0;
		u32 m_pair_type = 0;

		std::unordered_map<std::string, u32> m_symbol_cache;
		std::map<u32, CodeLabel> m_code_map;
	};

	Runtime& GetRuntime();
} // namespace Goal
