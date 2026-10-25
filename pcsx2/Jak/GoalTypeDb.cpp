// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalTypeDb.h"

#include "Host.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <charconv>

namespace Goal
{
	//////////////////////////////////////////////////////////////////////////
	// S-expressions
	//////////////////////////////////////////////////////////////////////////

	namespace
	{
		class Reader
		{
		public:
			explicit Reader(std::string_view src)
				: m_src(src)
			{
			}

			bool ReadAll(std::vector<SExpr>* out, std::string* error)
			{
				for (;;)
				{
					SkipWhitespaceAndComments();
					if (m_pos >= m_src.size())
						return true;
					SExpr expr;
					if (!ReadExpr(&expr, error))
						return false;
					out->push_back(std::move(expr));
				}
			}

		private:
			void SkipWhitespaceAndComments()
			{
				while (m_pos < m_src.size())
				{
					const char c = m_src[m_pos];
					if (c == ';')
					{
						while (m_pos < m_src.size() && m_src[m_pos] != '\n')
							m_pos++;
					}
					else if (c == '#' && m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '|')
					{
						const size_t end = m_src.find("|#", m_pos + 2);
						m_pos = (end == std::string_view::npos) ? m_src.size() : end + 2;
					}
					else if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
					{
						m_pos++;
					}
					else
					{
						break;
					}
				}
			}

			// looks for "; <number>" on the rest of the current line
			s32 PeekTrailingNumber() const
			{
				size_t p = m_pos;
				while (p < m_src.size() && (m_src[p] == ' ' || m_src[p] == '\t'))
					p++;
				if (p >= m_src.size() || m_src[p] != ';')
					return -1;
				while (p < m_src.size() && (m_src[p] == ';' || m_src[p] == ' ' || m_src[p] == '\t'))
					p++;
				size_t end = p;
				while (end < m_src.size() && m_src[end] >= '0' && m_src[end] <= '9')
					end++;
				if (end == p || (end < m_src.size() && m_src[end] != '\n' && m_src[end] != ' ' && m_src[end] != '\r'))
					return -1;
				s32 value = -1;
				std::from_chars(m_src.data() + p, m_src.data() + end, value);
				return value;
			}

			bool ReadExpr(SExpr* out, std::string* error)
			{
				SkipWhitespaceAndComments();
				if (m_pos >= m_src.size())
				{
					*error = "unexpected end of input";
					return false;
				}

				const char c = m_src[m_pos];
				if (c == '(')
				{
					m_pos++;
					out->kind = SExpr::Kind::List;
					for (;;)
					{
						SkipWhitespaceAndComments();
						if (m_pos >= m_src.size())
						{
							*error = "unterminated list";
							return false;
						}
						if (m_src[m_pos] == ')')
						{
							m_pos++;
							break;
						}
						SExpr item;
						if (!ReadExpr(&item, error))
							return false;
						out->items.push_back(std::move(item));
					}
					out->trailing_number = PeekTrailingNumber();
					return true;
				}

				if (c == ')')
				{
					*error = fmt::format("unexpected ')' at offset {}", m_pos);
					return false;
				}

				if (c == '"')
				{
					m_pos++;
					out->kind = SExpr::Kind::String;
					while (m_pos < m_src.size() && m_src[m_pos] != '"')
					{
						if (m_src[m_pos] == '\\' && m_pos + 1 < m_src.size())
							m_pos++;
						out->text.push_back(m_src[m_pos++]);
					}
					if (m_pos >= m_src.size())
					{
						*error = "unterminated string";
						return false;
					}
					m_pos++;
					return true;
				}

				if (c == '\'' || c == '`' || c == ',')
				{
					m_pos++;
					SExpr quoted;
					if (!ReadExpr(&quoted, error))
						return false;
					if (quoted.IsAtom())
					{
						out->kind = SExpr::Kind::Atom;
						out->text = "'" + quoted.text;
					}
					else
					{
						out->kind = SExpr::Kind::List;
						SExpr head;
						head.text = "quote";
						out->items.push_back(std::move(head));
						out->items.push_back(std::move(quoted));
					}
					return true;
				}

				out->kind = SExpr::Kind::Atom;
				while (m_pos < m_src.size())
				{
					const char a = m_src[m_pos];
					if (a == ' ' || a == '\t' || a == '\n' || a == '\r' || a == '(' || a == ')' || a == ';' || a == '"')
						break;
					out->text.push_back(a);
					m_pos++;
				}
				out->trailing_number = PeekTrailingNumber();
				return true;
			}

			std::string_view m_src;
			size_t m_pos = 0;
		};
	} // namespace

	bool ParseSExprs(std::string_view source, std::vector<SExpr>* out, std::string* error)
	{
		Reader reader(source);
		return reader.ReadAll(out, error);
	}

	std::optional<s64> ParseInteger(std::string_view atom)
	{
		bool negative = false;
		if (!atom.empty() && atom[0] == '-')
		{
			negative = true;
			atom.remove_prefix(1);
		}

		int base = 10;
		if (atom.starts_with("#x") || atom.starts_with("0x") || atom.starts_with("#X") || atom.starts_with("0X"))
		{
			base = 16;
			atom.remove_prefix(2);
		}
		else if (atom.starts_with("#b"))
		{
			base = 2;
			atom.remove_prefix(2);
		}

		if (atom.empty())
			return std::nullopt;

		u64 value = 0;
		const auto [ptr, ec] = std::from_chars(atom.data(), atom.data() + atom.size(), value, base);
		if (ec != std::errc() || ptr != atom.data() + atom.size())
			return std::nullopt;
		return negative ? -static_cast<s64>(value) : static_cast<s64>(value);
	}

	std::string SExpr::Print() const
	{
		switch (kind)
		{
			case Kind::Atom:
				return text;
			case Kind::String:
				return fmt::format("\"{}\"", text);
			case Kind::List:
			default:
			{
				std::string out = "(";
				for (size_t i = 0; i < items.size(); i++)
				{
					if (i != 0)
						out += ' ';
					out += items[i].Print();
				}
				out += ')';
				return out;
			}
		}
	}

	std::string MethodDef::Signature() const
	{
		std::string out = "(";
		for (size_t i = 0; i < args.size(); i++)
		{
			if (i != 0)
				out += ' ';
			out += args[i];
		}
		out += ") ";
		out += ret;
		return out;
	}

	//////////////////////////////////////////////////////////////////////////
	// Type database
	//////////////////////////////////////////////////////////////////////////

	namespace
	{
		struct Primitive
		{
			const char* name;
			const char* parent;
			TypeInfo::Category category;
			u32 size;
			bool is_signed;
		};

		constexpr Primitive s_primitives[] = {
			{"number", "object", TypeInfo::Category::Integer, 8, true},
			{"float", "number", TypeInfo::Category::Float, 4, true},
			{"integer", "number", TypeInfo::Category::Integer, 8, true},
			{"binteger", "integer", TypeInfo::Category::Integer, 8, true},
			{"sinteger", "integer", TypeInfo::Category::Integer, 8, true},
			{"uinteger", "integer", TypeInfo::Category::Integer, 8, false},
			{"int8", "sinteger", TypeInfo::Category::Integer, 1, true},
			{"int16", "sinteger", TypeInfo::Category::Integer, 2, true},
			{"int32", "sinteger", TypeInfo::Category::Integer, 4, true},
			{"int64", "sinteger", TypeInfo::Category::Integer, 8, true},
			{"int128", "sinteger", TypeInfo::Category::Integer, 16, true},
			{"int", "sinteger", TypeInfo::Category::Integer, 8, true},
			{"uint8", "uinteger", TypeInfo::Category::Integer, 1, false},
			{"uint16", "uinteger", TypeInfo::Category::Integer, 2, false},
			{"uint32", "uinteger", TypeInfo::Category::Integer, 4, false},
			{"uint64", "uinteger", TypeInfo::Category::Integer, 8, false},
			{"uint128", "uinteger", TypeInfo::Category::Integer, 16, false},
			{"uint", "uinteger", TypeInfo::Category::Integer, 8, false},
		};

		const Primitive* FindPrimitive(std::string_view name)
		{
			for (const Primitive& p : s_primitives)
			{
				if (name == p.name)
					return &p;
			}
			return nullptr;
		}

		u32 Align(u32 value, u32 alignment)
		{
			return alignment ? (value + alignment - 1) / alignment * alignment : value;
		}

		SExpr MakeAtom(std::string_view text)
		{
			SExpr e;
			e.kind = SExpr::Kind::Atom;
			e.text = std::string(text);
			return e;
		}
	} // namespace

	TypeDb& GetTypeDb()
	{
		static TypeDb s_db;
		static bool s_tried_settings = false;
		if (!s_tried_settings)
		{
			s_tried_settings = true;
			const std::string path = Host::GetBaseStringSettingValue("JakDebug", "AllTypesPath");
			std::string error;
			if (!path.empty() && !s_db.Load(path, &error))
				Console.ErrorFmt("(GoalTypeDb) {}", error);
		}
		return s_db;
	}

	void TypeDb::Clear()
	{
		m_loaded = false;
		m_path.clear();
		m_types.clear();
		m_enums.clear();
		m_globals.clear();
	}

	void TypeDb::AddBuiltins()
	{
		const auto add = [this](const char* name, const char* parent, u32 size, std::initializer_list<std::tuple<const char*, const char*, s32, bool>> fields) {
			auto type = std::make_unique<TypeDef>();
			type->name = name;
			type->parent = parent;
			type->builtin = true;
			type->size_assert = size;
			for (const auto& [fname, ftype, offset, dynamic] : fields)
			{
				FieldDef f;
				f.name = fname;
				f.type = MakeAtom(ftype);
				f.type_name = ftype;
				f.offset = offset;
				f.is_dynamic = dynamic;
				type->fields.push_back(std::move(f));
			}
			m_types[name] = std::move(type);
		};

		add("object", "", 4, {});
		add("structure", "object", 0, {});
		add("basic", "structure", 4, {{"type", "type", 0, false}});
		add("symbol", "object", 4, {});
		add("pair", "object", 8, {});
		add("function", "basic", 4, {});
		add("type", "basic", 20,
			{{"symbol", "symbol", 4, false}, {"parent", "type", 8, false}, {"size", "uint16", 12, false},
				{"psize", "uint16", 14, false}, {"heap-base", "uint16", 16, false}, {"allocated-length", "uint16", 18, false},
				{"method-table", "function", 20, true}});
		add("string", "basic", 8, {{"allocated-length", "uint32", 4, false}, {"data", "uint8", 8, true}});
		add("array", "basic", 16,
			{{"length", "int32", 4, false}, {"allocated-length", "int32", 8, false}, {"content-type", "type", 12, false},
				{"data", "uint8", 16, true}});
		add("kheap", "structure", 16,
			{{"base", "pointer", 0, false}, {"top", "pointer", 4, false}, {"current", "pointer", 8, false}, {"top-base", "pointer", 12, false}});
		add("pointer", "object", 4, {});
		add("inline-array", "object", 4, {});

		for (const Primitive& p : s_primitives)
			add(p.name, p.parent, p.size, {});
	}

	bool TypeDb::Load(const std::string& path, std::string* error)
	{
		std::optional<std::string> source = FileSystem::ReadFileToString(path.c_str());
		if (!source.has_value())
		{
			*error = fmt::format("could not read '{}'", path);
			return false;
		}

		std::vector<SExpr> forms;
		if (!ParseSExprs(*source, &forms, error))
			return false;

		Clear();
		AddBuiltins();

		for (const SExpr& form : forms)
		{
			if (!form.IsList() || form.items.empty() || !form.items[0].IsAtom())
				continue;
			const std::string& head = form.items[0].text;
			if (head == "deftype")
			{
				ParseDeftype(form);
			}
			else if (head == "defenum")
			{
				ParseDefenum(form);
			}
			else if (head == "define-extern" && form.items.size() >= 3 && form.items[1].IsAtom())
			{
				m_globals[form.items[1].text] = form.items[2];
			}
			else if (head == "declare-type" && form.items.size() >= 3 && form.items[1].IsAtom() && form.items[2].IsAtom())
			{
				if (!m_types.contains(form.items[1].text))
				{
					auto type = std::make_unique<TypeDef>();
					type->name = form.items[1].text;
					type->parent = form.items[2].text;
					type->declared_only = true;
					m_types[type->name] = std::move(type);
				}
			}
		}

		ComputeLayouts();
		m_path = path;
		m_loaded = true;
		return true;
	}

	void TypeDb::ParseDeftype(const SExpr& form)
	{
		if (form.items.size() < 3 || !form.items[1].IsAtom())
			return;

		auto type = std::make_unique<TypeDef>();
		type->name = form.items[1].text;
		if (form.items[2].IsList() && !form.items[2].items.empty())
			type->parent = form.items[2].items[0].text;

		size_t i = 3;
		while (i < form.items.size() && form.items[i].IsString())
			type->doc = form.items[i++].text;

		if (i < form.items.size() && form.items[i].IsList())
		{
			for (const SExpr& f : form.items[i].items)
			{
				if (!f.IsList() || f.items.size() < 2 || !f.items[0].IsAtom())
					continue;
				FieldDef field;
				field.name = f.items[0].text;
				field.type = f.items[1];
				field.type_name = f.items[1].IsList() ? (f.items[1].items.empty() ? "" : f.items[1].items[0].text) : f.items[1].text;
				for (size_t j = 2; j < f.items.size(); j++)
				{
					const SExpr& opt = f.items[j];
					if (!opt.IsAtom())
						continue;
					const auto next_int = [&]() -> std::optional<s64> {
						if (j + 1 < f.items.size() && f.items[j + 1].IsAtom())
							return ParseInteger(f.items[++j].text);
						return std::nullopt;
					};
					if (opt.text == ":inline")
						field.is_inline = true;
					else if (opt.text == ":dynamic")
						field.is_dynamic = true;
					else if (opt.text == ":override")
						field.is_override = true;
					else if (opt.text == ":offset" || opt.text == ":offset-assert")
						field.offset = static_cast<s32>(next_int().value_or(-1));
					else if (opt.text == ":size")
						field.bit_size = static_cast<s32>(next_int().value_or(-1));
					else if (opt.text == ":score" || opt.text == ":decomp-as")
						j++;
					else if (const std::optional<s64> n = ParseInteger(opt.text))
						field.array_size = static_cast<s32>(*n);
				}
				type->fields.push_back(std::move(field));
			}
			i++;
		}

		for (; i < form.items.size(); i++)
		{
			const SExpr& opt = form.items[i];
			if (opt.IsAtom())
			{
				const auto next_int = [&]() -> std::optional<s64> {
					if (i + 1 < form.items.size() && form.items[i + 1].IsAtom())
						return ParseInteger(form.items[++i].text);
					return std::nullopt;
				};
				if (opt.text == ":size-assert")
					type->size_assert = static_cast<u32>(next_int().value_or(0));
				else if (opt.text == ":method-count-assert")
					type->method_count_assert = static_cast<u32>(next_int().value_or(0));
				else if (opt.text == ":pack-me")
					type->pack_me = true;
				else if (opt.text == ":flag-assert" || opt.text == ":heap-base")
					i++;
				continue;
			}

			if (!opt.IsList() || opt.items.empty())
				continue;

			const std::string& section = opt.items[0].text;
			const bool is_states = (section == ":states");
			if (section != ":methods" && !is_states && section != ":state-methods")
				continue;

			for (size_t m = 1; m < opt.items.size(); m++)
			{
				const SExpr& entry = opt.items[m];
				MethodDef method;
				method.id = entry.trailing_number;
				method.is_state = is_states;
				if (entry.IsAtom())
				{
					method.name = entry.text;
					method.is_state = true;
				}
				else if (entry.IsList() && !entry.items.empty())
				{
					method.name = entry.items[0].text;
					size_t k = 1;
					if (k < entry.items.size() && entry.items[k].IsString())
						method.doc = entry.items[k++].text;
					if (!is_states && k < entry.items.size() && entry.items[k].IsList())
					{
						for (const SExpr& arg : entry.items[k].items)
							method.args.push_back(arg.Print());
						k++;
						if (k < entry.items.size())
							method.ret = entry.items[k].Print();
					}
					else
					{
						for (; k < entry.items.size(); k++)
							method.args.push_back(entry.items[k].Print());
					}
				}
				else
				{
					continue;
				}
				type->methods.push_back(std::move(method));
			}
		}

		m_types[type->name] = std::move(type);
	}

	void TypeDb::ParseDefenum(const SExpr& form)
	{
		if (form.items.size() < 2 || !form.items[1].IsAtom())
			return;

		EnumDef def;
		def.name = form.items[1].text;
		s64 next = 0;
		for (size_t i = 2; i < form.items.size(); i++)
		{
			const SExpr& item = form.items[i];
			if (item.IsAtom())
			{
				if (i + 1 >= form.items.size())
					break;
				const SExpr& value = form.items[++i];
				if (item.text == ":type")
					def.type = value.text;
				else if (item.text == ":bitfield")
					def.bitfield = value.IsAtom("#t");
				else if (item.text == ":copy-entries")
				{
					if (const auto it = m_enums.find(value.text); it != m_enums.end())
						def.entries.insert(def.entries.end(), it->second.entries.begin(), it->second.entries.end());
				}
				continue;
			}
			if (!item.IsList() || item.items.empty())
				continue;
			s64 value = next;
			if (item.items.size() >= 2)
				value = ParseInteger(item.items[1].text).value_or(next);
			def.entries.emplace_back(item.items[0].text, value);
			next = value + 1;
		}
		m_enums[def.name] = std::move(def);
	}

	void TypeDb::ComputeLayouts()
	{
		for (auto& [name, type] : m_types)
			ComputeLayout(*type);
	}

	void TypeDb::ComputeLayout(TypeDef& type)
	{
		if (type.layout_done || type.layout_in_progress)
			return;
		type.layout_in_progress = true;

		u32 size = 0;
		u32 method_count = 9;
		TypeDef* parent = nullptr;
		if (const auto it = m_types.find(type.parent); it != m_types.end() && it->second.get() != &type)
		{
			parent = it->second.get();
			ComputeLayout(*parent);
			size = parent->size;
			method_count = parent->method_count;
		}

		const TypeInfo self_info = Info(type.name);
		const bool is_value = self_info.category == TypeInfo::Category::Integer || self_info.category == TypeInfo::Category::Float ||
							  self_info.category == TypeInfo::Category::Bitfield;
		if (!is_value)
		{
			for (FieldDef& field : type.fields)
			{
				if (field.is_inline)
				{
					if (const auto it = m_types.find(field.type_name); it != m_types.end())
						ComputeLayout(*it->second);
				}

				if (field.offset < 0 && field.is_override && parent)
				{
					if (const FieldDef* overridden = FindField(parent->name, field.name))
						field.offset = overridden->offset;
				}

				if (field.offset < 0)
				{
					field.offset = static_cast<s32>(Align(size, FieldAlignment(field)));
					field.offset_computed = true;
				}
				size = std::max(size, static_cast<u32>(field.offset) + FieldSize(field));
			}
		}
		else
		{
			size = self_info.size;
		}

		type.size = type.size_assert.value_or(size);

		u32 next_id = method_count;
		for (MethodDef& method : type.methods)
		{
			if (method.id < 0)
			{
				if (method.name == "new")
					method.id = 0;
				else if (const MethodDef* inherited = parent ? FindMethodByName(parent->name, method.name) : nullptr)
					method.id = inherited->id;
				else
					method.id = static_cast<s32>(next_id);
			}
			next_id = std::max(next_id, static_cast<u32>(method.id) + 1);
		}
		type.method_count = type.method_count_assert.value_or(next_id);

		type.layout_in_progress = false;
		type.layout_done = true;
	}

	const TypeDef* TypeDb::FindType(std::string_view name) const
	{
		const auto it = m_types.find(std::string(name));
		return it != m_types.end() ? it->second.get() : nullptr;
	}

	const EnumDef* TypeDb::FindEnum(std::string_view name) const
	{
		const auto it = m_enums.find(std::string(name));
		return it != m_enums.end() ? &it->second : nullptr;
	}

	const SExpr* TypeDb::FindGlobal(std::string_view name) const
	{
		const auto it = m_globals.find(std::string(name));
		return it != m_globals.end() ? &it->second : nullptr;
	}

	std::optional<FunctionSig> TypeDb::FunctionSignature(const SExpr& spec) const
	{
		if (!spec.IsList() || spec.items.empty() || !spec.items[0].IsAtom("function"))
			return std::nullopt;

		FunctionSig sig;
		std::vector<SExpr> parts;
		for (size_t i = 1; i < spec.items.size(); i++)
		{
			if (spec.items[i].IsAtom(":behavior") && i + 1 < spec.items.size())
			{
				sig.behavior = spec.items[++i].text;
				continue;
			}
			parts.push_back(spec.items[i]);
		}
		if (parts.empty())
			return sig;
		sig.ret = parts.back();
		parts.pop_back();
		sig.args = std::move(parts);
		return sig;
	}

	std::vector<std::string> TypeDb::ParentChain(std::string_view type) const
	{
		std::vector<std::string> chain;
		std::string current(type);
		for (int depth = 0; depth < 64 && !current.empty(); depth++)
		{
			chain.push_back(current);
			const TypeDef* def = FindType(current);
			if (!def || def->parent == current)
				break;
			current = def->parent;
		}
		return chain;
	}

	bool TypeDb::Inherits(std::string_view type, std::string_view ancestor) const
	{
		for (const std::string& t : ParentChain(type))
		{
			if (t == ancestor)
				return true;
		}
		return false;
	}

	std::vector<const FieldDef*> TypeDb::AllFields(std::string_view type) const
	{
		std::vector<std::string> chain = ParentChain(type);
		std::reverse(chain.begin(), chain.end());

		std::vector<const FieldDef*> fields;
		for (const std::string& t : chain)
		{
			const TypeDef* def = FindType(t);
			if (!def)
				continue;
			for (const FieldDef& field : def->fields)
			{
				auto existing = std::find_if(fields.begin(), fields.end(), [&](const FieldDef* f) { return f->name == field.name; });
				if (existing != fields.end())
					*existing = &field;
				else
					fields.push_back(&field);
			}
		}
		return fields;
	}

	const FieldDef* TypeDb::FindField(std::string_view type, std::string_view field) const
	{
		for (const FieldDef* f : AllFields(type))
		{
			if (f->name == field)
				return f;
		}
		return nullptr;
	}

	const MethodDef* TypeDb::FindMethod(std::string_view type, s32 id) const
	{
		for (const std::string& t : ParentChain(type))
		{
			const TypeDef* def = FindType(t);
			if (!def)
				continue;
			for (const MethodDef& m : def->methods)
			{
				if (m.id == id)
					return &m;
			}
		}
		return nullptr;
	}

	const MethodDef* TypeDb::FindMethodByName(std::string_view type, std::string_view name) const
	{
		for (const std::string& t : ParentChain(type))
		{
			const TypeDef* def = FindType(t);
			if (!def)
				continue;
			for (const MethodDef& m : def->methods)
			{
				if (m.name == name)
					return &m;
			}
		}
		return nullptr;
	}

	u32 TypeDb::MethodCount(std::string_view type) const
	{
		const TypeDef* def = FindType(type);
		return def ? def->method_count : 0;
	}

	u32 TypeDb::Size(std::string_view type) const
	{
		const TypeDef* def = FindType(type);
		return def ? def->size : 0;
	}

	TypeInfo TypeDb::Info(const SExpr& spec) const
	{
		if (spec.IsList())
		{
			const std::string head = spec.items.empty() ? std::string() : spec.items[0].text;
			TypeInfo info;
			info.name = spec.Print();
			if (head == "pointer" || head == "inline-array")
				info.category = TypeInfo::Category::Pointer;
			else if (head == "function" || head == "state")
				info.category = TypeInfo::Category::Function;
			else if (!head.empty())
			{
				// (array foo) and friends behave like their head type
				TypeInfo head_info = Info(head);
				head_info.name = info.name;
				return head_info;
			}
			return info;
		}
		return Info(spec.text);
	}

	TypeInfo TypeDb::Info(std::string_view type_name) const
	{
		TypeInfo info;
		info.name = std::string(type_name);

		if (type_name == "symbol")
		{
			info.category = TypeInfo::Category::Symbol;
			return info;
		}
		if (type_name == "pointer" || type_name == "inline-array")
		{
			info.category = TypeInfo::Category::Pointer;
			return info;
		}
		if (type_name == "function")
		{
			info.category = TypeInfo::Category::Function;
			return info;
		}
		if (type_name == "object" || type_name.empty())
			return info;

		if (const EnumDef* e = FindEnum(type_name))
		{
			TypeInfo base = Info(e->type);
			base.category = TypeInfo::Category::Enum;
			base.name = info.name;
			return base;
		}

		bool has_fields = false;
		for (const std::string& t : ParentChain(type_name))
		{
			if (const Primitive* p = FindPrimitive(t))
			{
				info.category = has_fields ? TypeInfo::Category::Bitfield : p->category;
				info.size = p->size;
				info.align = std::min<u32>(p->size, 16);
				info.is_signed = p->is_signed;
				return info;
			}
			if (const TypeDef* def = FindType(t); def && !def->fields.empty())
				has_fields = true;
			if (t == "structure" || t == "basic")
			{
				info.category = TypeInfo::Category::Reference;
				info.is_basic = Inherits(type_name, "basic");
				return info;
			}
		}
		return info;
	}

	u32 TypeDb::InlineStride(std::string_view type) const
	{
		const TypeDef* def = FindType(type);
		if (!def)
			return 16;
		return Align(def->size, def->pack_me ? 4 : 16);
	}

	u32 TypeDb::FieldAlignment(const FieldDef& field) const
	{
		if (field.is_inline)
		{
			const TypeDef* def = FindType(field.type_name);
			return (def && def->pack_me) ? 4 : 16;
		}
		const TypeInfo info = Info(field.type);
		if (info.category == TypeInfo::Category::Reference || info.category == TypeInfo::Category::Unknown)
			return 4;
		return info.align;
	}

	u32 TypeDb::FieldSize(const FieldDef& field) const
	{
		if (field.is_dynamic)
			return 0;

		const u32 count = field.IsArray() ? static_cast<u32>(field.array_size) : 1;
		if (field.is_inline)
			return field.IsArray() ? count * InlineStride(field.type_name) : Size(field.type_name);

		const TypeInfo info = Info(field.type);
		const u32 elem = (info.category == TypeInfo::Category::Reference || info.category == TypeInfo::Category::Unknown) ? 4 : info.size;
		return count * elem;
	}

	std::vector<std::string> TypeDb::TypeNames() const
	{
		std::vector<std::string> names;
		names.reserve(m_types.size());
		for (const auto& [name, def] : m_types)
			names.push_back(name);
		std::sort(names.begin(), names.end());
		return names;
	}
} // namespace Goal
