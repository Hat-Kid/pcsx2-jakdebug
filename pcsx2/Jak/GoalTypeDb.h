// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Goal
{
	struct SExpr
	{
		enum class Kind : u8
		{
			List,
			Atom,
			String,
		};

		Kind kind = Kind::Atom;
		std::string text;
		std::vector<SExpr> items;
		// the number in a ";; 12" comment directly following a list, used for method ids
		s32 trailing_number = -1;

		bool IsList() const { return kind == Kind::List; }
		bool IsAtom() const { return kind == Kind::Atom; }
		bool IsAtom(std::string_view s) const { return kind == Kind::Atom && text == s; }
		bool IsString() const { return kind == Kind::String; }
		std::string Print() const;
	};

	bool ParseSExprs(std::string_view source, std::vector<SExpr>* out, std::string* error);
	std::optional<s64> ParseInteger(std::string_view atom);

	struct FieldDef
	{
		std::string name;
		SExpr type;
		std::string type_name; // head of the type spec, e.g. "pointer" for (pointer vector)
		s32 offset = -1;       // bits for bitfield types, bytes otherwise
		s32 bit_size = -1;     // bitfield types only
		s32 array_size = -1;
		bool is_inline = false;
		bool is_dynamic = false;
		bool is_override = false;
		bool offset_computed = false;

		bool IsArray() const { return array_size >= 0; }
		std::string TypeText() const { return type.Print(); }
	};

	struct MethodDef
	{
		std::string name;
		s32 id = -1;
		std::string doc;
		std::vector<std::string> args;
		std::string ret;
		bool is_state = false;

		std::string Signature() const;
	};

	struct TypeDef
	{
		std::string name;
		std::string parent;
		std::string doc;
		std::vector<FieldDef> fields; // own fields only
		std::vector<MethodDef> methods;
		std::optional<u32> size_assert;
		std::optional<u32> method_count_assert;
		bool pack_me = false;
		bool builtin = false;
		bool declared_only = false;

		// filled by the layout pass
		bool layout_done = false;
		bool layout_in_progress = false;
		u32 size = 0;
		u32 method_count = 0;
	};

	struct EnumDef
	{
		std::string name;
		std::string type = "int32";
		bool bitfield = false;
		std::vector<std::pair<std::string, s64>> entries;
	};

	struct FunctionSig
	{
		std::vector<SExpr> args;
		SExpr ret;
		std::string behavior;
	};

	// what a type spec means for memory layout and printing
	struct TypeInfo
	{
		enum class Category : u8
		{
			Unknown,
			Integer,
			Float,
			Enum,
			Bitfield,
			Symbol,
			Pointer, // pointer, (pointer x), (inline-array x)
			Function,
			Reference, // structure/basic reference
		};

		Category category = Category::Unknown;
		u32 size = 4;
		u32 align = 4;
		bool is_signed = false;
		bool is_basic = false; // only for references
		std::string name;
	};

	class TypeDb
	{
	public:
		bool Load(const std::string& path, std::string* error);
		bool IsLoaded() const { return m_loaded; }
		const std::string& Path() const { return m_path; }
		void Clear();

		size_t TypeCount() const { return m_types.size(); }
		size_t EnumCount() const { return m_enums.size(); }
		size_t GlobalCount() const { return m_globals.size(); }

		const TypeDef* FindType(std::string_view name) const;
		const EnumDef* FindEnum(std::string_view name) const;
		const SExpr* FindGlobal(std::string_view name) const;
		std::optional<FunctionSig> FunctionSignature(const SExpr& spec) const;

		bool Inherits(std::string_view type, std::string_view ancestor) const;
		std::vector<std::string> ParentChain(std::string_view type) const;
		bool IsBasic(std::string_view type) const { return Inherits(type, "basic"); }
		bool IsStructure(std::string_view type) const { return Inherits(type, "structure"); }

		// fields from the root down; overridden fields are replaced by their override
		std::vector<const FieldDef*> AllFields(std::string_view type) const;
		const FieldDef* FindField(std::string_view type, std::string_view field) const;

		const MethodDef* FindMethod(std::string_view type, s32 id) const;
		const MethodDef* FindMethodByName(std::string_view type, std::string_view name) const;
		u32 MethodCount(std::string_view type) const;
		u32 Size(std::string_view type) const;

		TypeInfo Info(const SExpr& spec) const;
		TypeInfo Info(std::string_view type_name) const;
		u32 FieldSize(const FieldDef& field) const;
		u32 InlineStride(std::string_view type) const;

		std::vector<std::string> TypeNames() const;

	private:
		void AddBuiltins();
		void ParseDeftype(const SExpr& form);
		void ParseDefenum(const SExpr& form);
		void ComputeLayouts();
		void ComputeLayout(TypeDef& type);
		u32 FieldAlignment(const FieldDef& field) const;

		bool m_loaded = false;
		std::string m_path;
		std::unordered_map<std::string, std::unique_ptr<TypeDef>> m_types;
		std::unordered_map<std::string, EnumDef> m_enums;
		std::unordered_map<std::string, SExpr> m_globals;
	};

	TypeDb& GetTypeDb();
} // namespace Goal
