// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalInspector.h"
#include "GoalRuntime.h"
#include "GoalTypeDb.h"

#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_set>

namespace Goal::Inspector
{
	namespace
	{
		constexpr u32 MAX_ARRAY_PRINT = 16;
		constexpr u32 TICKS_PER_SECOND = 300;

		SExpr Atom(std::string_view text)
		{
			SExpr e;
			e.kind = SExpr::Kind::Atom;
			e.text = std::string(text);
			return e;
		}

		std::string Indent(const std::string& text, std::string_view prefix)
		{
			std::string out;
			size_t start = 0;
			while (start < text.size())
			{
				size_t end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				out += prefix;
				out.append(text, start, end - start);
				out += '\n';
				start = end + 1;
			}
			return out;
		}

		std::string Truncate(std::string text, size_t len)
		{
			if (text.size() > len)
			{
				text.resize(len - 1);
				text += "~";
			}
			return text;
		}

		bool InheritsName(std::string_view type, std::string_view ancestor)
		{
			return GetTypeDb().Inherits(type, ancestor);
		}

		// fields sorted by offset, larger first when they start at the same place
		std::vector<const FieldDef*> SortedFields(std::string_view type)
		{
			std::vector<const FieldDef*> fields = GetTypeDb().AllFields(type);
			std::stable_sort(fields.begin(), fields.end(), [](const FieldDef* a, const FieldDef* b) {
				return a->offset < b->offset;
			});
			return fields;
		}

		const FieldDef* FieldAt(std::string_view type, u32 offset)
		{
			const TypeDb& db = GetTypeDb();
			const FieldDef* best = nullptr;
			u32 best_size = UINT32_MAX;
			for (const FieldDef* f : db.AllFields(type))
			{
				if (f->offset < 0)
					continue;
				const u32 start = static_cast<u32>(f->offset);
				const u32 size = std::max<u32>(db.FieldSize(*f), f->is_dynamic ? UINT32_MAX - start : 1);
				if (offset < start || offset - start >= size)
					continue;
				// prefer the most specific field (x over data, etc.)
				if (size < best_size)
				{
					best = f;
					best_size = size;
				}
			}
			return best;
		}

		std::string FieldLabel(std::string_view type, u32 offset)
		{
			const TypeDb& db = GetTypeDb();
			std::string path;
			std::string type_text;
			std::string current(type);
			// descend into inline structures and inline arrays, offsets inside them are relative to their start
			for (int depth = 0; depth < 8 && !current.empty(); depth++)
			{
				const FieldDef* f = FieldAt(current, offset);
				if (!f)
					break;
				offset -= static_cast<u32>(f->offset);
				path += (path.empty() ? "" : ".") + f->name;
				type_text = f->TypeText();
				current.clear();
				if (!f->is_inline)
					break;
				if (f->IsArray() || f->is_dynamic)
				{
					const u32 stride = db.InlineStride(f->type_name);
					if (stride == 0)
						break;
					path += fmt::format("[{}]", offset / stride);
					offset %= stride;
				}
				if (db.IsStructure(f->type_name))
				{
					current = f->type_name;
					type_text = current;
				}
			}
			if (path.empty())
				return {};
			if (offset == 0)
				return fmt::format("{} {}", path, type_text);
			return fmt::format("{}+{} {}", path, offset, type_text);
		}

		// handles are (pointer process) + pid packed in 64 bits
		std::optional<u32> HandleProcess(u64 value)
		{
			Runtime& rt = GetRuntime();
			const u32 ppointer = static_cast<u32>(value);
			const u32 pid = static_cast<u32>(value >> 32);
			if (!Mem::IsRam(ppointer) || ppointer == 0)
				return std::nullopt;
			const u32 process = Mem::Read32(ppointer);
			const u32 type = rt.BasicType(process);
			if (!type || !rt.TypeInherits(type, "process"))
				return std::nullopt;
			if (const FieldDef* pid_field = GetTypeDb().FindField("process", "pid"))
			{
				if (Mem::Read32(process - 4 + pid_field->offset) != pid)
					return std::nullopt;
			}
			return process;
		}

		std::string Guessed(u32 word, u32 next_word, bool aligned8)
		{
			Runtime& rt = GetRuntime();
			const ValueInfo info = rt.ClassifyWord(word);
			std::string text = rt.DescribeInfo(info, static_cast<u64>(word), true);
			switch (info.kind)
			{
				case ValueKind::Float:
				{
					const float f = std::bit_cast<float>(word);
					if (std::fabs(f) >= 409.6f && std::fabs(f) < 4096.0f * 100000.0f)
						text += fmt::format("  (meters {:.4g}?)", f / 4096.0f);
					break;
				}
				case ValueKind::Pointer:
				{
					// a pointer to a pointer to a process with a matching pid next to it is a handle
					if (aligned8)
					{
						if (const std::optional<u32> proc = HandleProcess(static_cast<u64>(word) | (static_cast<u64>(next_word) << 32)))
							text = fmt::format("handle -> {}", rt.Describe(*proc));
					}
					const u32 target = Mem::Read32(word);
					if (text.starts_with("#x") && rt.IsBasic(target) && Mem::Read32(target - 4) != 0)
						text += fmt::format("  -> {}", rt.Describe(target));
					break;
				}
				default:
					break;
			}
			return text;
		}

		struct WordStats
		{
			std::map<ValueKind, u32> kinds;
			std::map<u32, u32> basic_types;
			std::map<u32, u32> symbols;
			std::set<u32> distinct;
			float fmin = INFINITY, fmax = -INFINITY;
			s32 imin = INT32_MAX, imax = INT32_MIN;
			u32 total = 0;

			void Add(u32 word)
			{
				Runtime& rt = GetRuntime();
				const ValueInfo info = rt.ClassifyWord(word);
				kinds[info.kind]++;
				total++;
				if (distinct.size() < 64)
					distinct.insert(word);
				switch (info.kind)
				{
					case ValueKind::Basic:
						basic_types[info.detail]++;
						break;
					case ValueKind::Symbol:
					case ValueKind::False:
					case ValueKind::True:
						symbols[word]++;
						break;
					case ValueKind::Float:
						fmin = std::min(fmin, std::bit_cast<float>(word));
						fmax = std::max(fmax, std::bit_cast<float>(word));
						break;
					case ValueKind::Int:
						imin = std::min(imin, static_cast<s32>(word));
						imax = std::max(imax, static_cast<s32>(word));
						break;
					default:
						break;
				}
			}

			std::string Summary() const
			{
				Runtime& rt = GetRuntime();
				if (total == 0)
					return {};
				if (distinct.size() == 1 && total > 1)
					return fmt::format("always {}", rt.Describe(static_cast<u64>(static_cast<s64>(static_cast<s32>(*distinct.begin()))), true));

				std::vector<std::pair<u32, ValueKind>> sorted;
				for (const auto& [kind, count] : kinds)
					sorted.emplace_back(count, kind);
				std::sort(sorted.rbegin(), sorted.rend());

				std::string out;
				for (const auto& [count, kind] : sorted)
				{
					if (!out.empty())
						out += " | ";
					const u32 pct = count * 100 / total;
					switch (kind)
					{
						case ValueKind::Float:
							out += fmt::format("float {}% [{:.4g}, {:.4g}]", pct, fmin, fmax);
							break;
						case ValueKind::Int:
							out += fmt::format("int {}% [{}, {}]", pct, imin, imax);
							break;
						case ValueKind::Basic:
						{
							std::string types;
							for (const auto& [type, n] : basic_types)
								types += fmt::format("{}{}", types.empty() ? "" : " ", rt.TypeName(type));
							out += fmt::format("basic {}% ({})", pct, Truncate(types, 60));
							break;
						}
						case ValueKind::Symbol:
						case ValueKind::False:
						case ValueKind::True:
						{
							if (kind != ValueKind::Symbol)
							{
								out += fmt::format("{} {}%", kind == ValueKind::False ? "#f" : "#t", pct);
								break;
							}
							std::string syms;
							for (const auto& [sym, n] : symbols)
							{
								if (sym != rt.FalseSym() && sym != rt.TrueSym())
									syms += fmt::format("{}'{}", syms.empty() ? "" : " ", rt.SymbolName(sym));
							}
							out += fmt::format("symbol {}% ({})", pct, Truncate(syms, 60));
							break;
						}
						case ValueKind::Zero:
							out += fmt::format("0 {}%", pct);
							break;
						case ValueKind::Pointer:
							out += fmt::format("pointer {}%", pct);
							break;
						case ValueKind::ScratchPointer:
							out += fmt::format("scratchpad-ptr {}%", pct);
							break;
						case ValueKind::Function:
							out += fmt::format("function {}%", pct);
							break;
						case ValueKind::String:
							out += fmt::format("string {}%", pct);
							break;
						case ValueKind::Type:
							out += fmt::format("type {}%", pct);
							break;
						case ValueKind::Pair:
						case ValueKind::EmptyPair:
							out += fmt::format("pair {}%", pct);
							break;
						default:
							out += fmt::format("other {}%", pct);
							break;
					}
				}
				return out;
			}

			// a more specific type than the declared one, if the data is unambiguous
			std::string Suggestion(std::string_view declared) const
			{
				static constexpr std::string_view generic[] = {"uint32", "int32", "uint64", "int64", "basic", "object", "pointer",
					"structure", "uint8", "int", "uint"};
				if (total < 2 || std::find(std::begin(generic), std::end(generic), declared) == std::end(generic))
					return {};
				Runtime& rt = GetRuntime();
				const auto only = [&](ValueKind kind, u32 allow_zero) {
					u32 n = 0;
					for (const auto& [k, c] : kinds)
					{
						if (k == kind || (allow_zero && k == ValueKind::Zero))
							n += c;
					}
					return n == total && kinds.contains(kind);
				};
				if (only(ValueKind::Float, true) && declared != "float")
					return "float";
				if (only(ValueKind::Basic, false) || (only(ValueKind::Basic, true) && kinds.contains(ValueKind::Zero)))
				{
					if (basic_types.size() == 1)
						return rt.TypeName(basic_types.begin()->first);
					return "basic";
				}
				u32 symbolish = 0;
				for (ValueKind k : {ValueKind::Symbol, ValueKind::False, ValueKind::True})
					symbolish += kinds.contains(k) ? kinds.at(k) : 0;
				if (symbolish == total && declared != "symbol" && (kinds.contains(ValueKind::Symbol) || kinds.contains(ValueKind::True)))
					return "symbol";
				if (only(ValueKind::Function, true))
					return "function";
				if (only(ValueKind::String, true))
					return "string";
				return {};
			}
		};

		std::map<std::string, std::tuple<u32, std::string, std::vector<u8>>> s_snapshots;
	} // namespace

	std::string ResolveTypeName(u32 addr, std::string_view static_type)
	{
		if (!static_type.empty())
			return std::string(static_type);
		Runtime& rt = GetRuntime();
		if (const u32 type = rt.BasicType(addr))
			return rt.TypeName(type);
		return {};
	}

	u32 ObjectBase(u32 addr, std::string_view type)
	{
		const TypeDb& db = GetTypeDb();
		if (db.FindType(type))
			return db.IsBasic(type) ? addr - 4 : addr;
		return GetRuntime().IsBasic(addr) ? addr - 4 : addr;
	}

	u64 LoadTypedValue(const SExpr& type, u32 addr, u32 size, bool is_signed)
	{
		(void)type;
		switch (size)
		{
			case 1:
			{
				const u8 v = Mem::Read8(addr);
				return is_signed ? static_cast<u64>(static_cast<s64>(static_cast<s8>(v))) : v;
			}
			case 2:
			{
				const u16 v = Mem::Read16(addr);
				return is_signed ? static_cast<u64>(static_cast<s64>(static_cast<s16>(v))) : v;
			}
			case 4:
			{
				const u32 v = Mem::Read32(addr);
				return is_signed ? static_cast<u64>(static_cast<s64>(static_cast<s32>(v))) : v;
			}
			default:
				return Mem::Read64(addr);
		}
	}

	std::string FormatTypedValue(const SExpr& type, u64 value, u32 addr)
	{
		const TypeDb& db = GetTypeDb();
		Runtime& rt = GetRuntime();
		const TypeInfo info = db.Info(type);
		const std::string head = type.IsList() ? (type.items.empty() ? std::string() : type.items[0].text) : type.text;

		switch (info.category)
		{
			case TypeInfo::Category::Integer:
			{
				std::string text;
				if (info.size == 16 && addr)
					text = fmt::format("#x{:016x}{:016x}", Mem::Read64(addr + 8), Mem::Read64(addr));
				else if (info.is_signed)
					text = fmt::format("{}", static_cast<s64>(value));
				else
					text = fmt::format("{}", value);
				if (info.size < 16 && (value > 9999 && static_cast<s64>(value) > 9999))
					text += fmt::format(" (#x{:x})", value);
				if (InheritsName(head, "time-frame"))
					text += fmt::format(" ({:.2f}s)", static_cast<double>(static_cast<s64>(value)) / TICKS_PER_SECOND);
				return text;
			}
			case TypeInfo::Category::Float:
			{
				const float f = std::bit_cast<float>(static_cast<u32>(value));
				if (InheritsName(head, "meters"))
					return fmt::format("{} ({:.4g}m)", f, f / 4096.0f);
				if (InheritsName(head, "degrees"))
					return fmt::format("{} ({:.4g}deg)", f, f * 360.0f / 65536.0f);
				return fmt::format("{}", f);
			}
			case TypeInfo::Category::Enum:
			{
				const EnumDef* e = db.FindEnum(head);
				if (!e)
					return fmt::format("#x{:x}", value);
				if (e->bitfield)
				{
					std::string flags;
					u64 remaining = value;
					for (const auto& [name, bit] : e->entries)
					{
						if (bit >= 0 && bit < 64 && (value & (1ull << bit)))
						{
							flags += fmt::format("{}{}", flags.empty() ? "" : " ", name);
							remaining &= ~(1ull << bit);
						}
					}
					if (remaining)
						flags += fmt::format("{}#x{:x}", flags.empty() ? "" : " ", remaining);
					return fmt::format("#x{:x} ({})", value, flags);
				}
				for (const auto& [name, v] : e->entries)
				{
					if (static_cast<u64>(v) == value)
						return fmt::format("{} ({})", name, static_cast<s64>(value));
				}
				return fmt::format("{} (not in {})", static_cast<s64>(value), e->name);
			}
			case TypeInfo::Category::Bitfield:
			{
				std::string out = fmt::format("#x{:x}", value);
				if (InheritsName(head, "handle"))
				{
					if (value == 0)
						return "#f (null handle)";
					if (const std::optional<u32> proc = HandleProcess(value))
						return fmt::format("{} -> {}", out, rt.Describe(*proc));
					return out + " (stale handle)";
				}
				std::string parts;
				for (const FieldDef* f : db.AllFields(head))
				{
					const s32 bits = f->bit_size > 0 ? f->bit_size : static_cast<s32>(db.Info(f->type).size * 8);
					if (f->offset < 0 || f->offset >= 64)
						continue;
					const u64 mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
					parts += fmt::format("{}{}={}", parts.empty() ? "" : " ", f->name, (value >> f->offset) & mask);
				}
				return parts.empty() ? out : fmt::format("{} ({})", out, Truncate(parts, 120));
			}
			case TypeInfo::Category::Reference:
			{
				std::string text = rt.Describe(value, false);
				const u32 ptr = static_cast<u32>(value);
				if (!info.is_basic && Mem::Valid(ptr, 16) && ptr != 0 && InheritsName(head, "vector"))
				{
					text += fmt::format(" [{} {} {} {}]", Mem::ReadFloat(ptr), Mem::ReadFloat(ptr + 4), Mem::ReadFloat(ptr + 8),
						Mem::ReadFloat(ptr + 12));
				}
				// flag pointers that disagree with the declared type
				if (info.is_basic && rt.IsBasic(ptr) && db.FindType(head))
				{
					const std::string actual = rt.TypeName(rt.BasicType(ptr));
					if (!db.Inherits(actual, head) && !actual.empty())
						text += fmt::format("  !! declared {}", head);
				}
				return text;
			}
			case TypeInfo::Category::Pointer:
			case TypeInfo::Category::Unknown:
				return rt.Describe(value, true);
			case TypeInfo::Category::Symbol:
			case TypeInfo::Category::Function:
			default:
				return rt.Describe(value, false);
		}
	}

	std::string Dump(u32 addr, std::string_view type, u32 depth)
	{
		Runtime& rt = GetRuntime();
		const TypeDb& db = GetTypeDb();

		const std::string type_name = ResolveTypeName(addr, type);
		if (type_name.empty())
			return fmt::format("#x{:x} is not a basic, give a type: dump <addr> <type>\n", addr);

		const TypeDef* def = db.FindType(type_name);
		const u32 runtime_type = rt.BasicType(addr);
		if (!def)
		{
			std::string out = fmt::format("type {} is not in all-types.gc{}\n", type_name, db.IsLoaded() ? "" : " (none loaded, use 'types <path>')");
			if (runtime_type)
				out += Guess(addr, std::max<u32>(rt.TypeSize(runtime_type), 16), {});
			return out;
		}

		const u32 base = ObjectBase(addr, type_name);
		std::string out = fmt::format("[{:08x}] {}", addr, type_name);
		if (runtime_type && db.IsBasic(type_name))
		{
			const std::string runtime_name = rt.TypeName(runtime_type);
			if (runtime_name != type_name)
				out += fmt::format(" (runtime type {})", runtime_name);
			if (rt.TypeSize(runtime_type) != def->size && runtime_name == type_name)
				out += fmt::format(" (size: runtime #x{:x}, all-types #x{:x})", rt.TypeSize(runtime_type), def->size);
		}
		out += '\n';

		std::vector<bool> covered(def->size, false);
		if (db.IsBasic(type_name))
			std::fill_n(covered.begin(), std::min<size_t>(4, covered.size()), true);

		for (const FieldDef* f : SortedFields(type_name))
		{
			if (f->offset < 0 || (f->name == "type" && db.IsBasic(type_name) && f->offset == 0))
				continue;

			const u32 field_size = db.FieldSize(*f);
			for (u32 i = static_cast<u32>(f->offset); i < std::min<u32>(f->offset + field_size, def->size); i++)
				covered[i] = true;

			const u32 faddr = base + static_cast<u32>(f->offset);
			std::string line = fmt::format("  +{:#05x} {:<24} {:<22} ", f->offset, f->name, Truncate(f->TypeText(), 22));
			if (f->offset_computed)
				line[2] = '~';

			if (f->is_dynamic)
			{
				if (db.Inherits(type_name, "array") && f->name == "data" && runtime_type)
				{
					const u32 length = Mem::Read32(base + 4);
					const u32 content_type = Mem::Read32(base + 12);
					const std::string content = rt.TypeName(content_type);
					const TypeInfo cinfo = db.Info(content);
					const u32 elem = (cinfo.category == TypeInfo::Category::Reference || cinfo.category == TypeInfo::Category::Unknown) ? 4 : cinfo.size;
					line += fmt::format("{} x {}:", length, content);
					for (u32 i = 0; i < std::min(length, MAX_ARRAY_PRINT); i++)
					{
						const u64 v = LoadTypedValue(Atom(content), faddr + i * elem, elem, cinfo.is_signed);
						line += ' ' + FormatTypedValue(Atom(content), v, faddr + i * elem);
					}
					if (length > MAX_ARRAY_PRINT)
						line += " ...";
				}
				else
				{
					line += fmt::format("(dynamic, starts at #x{:x})", faddr);
				}
				out += line + '\n';
				continue;
			}

			if (f->is_inline)
			{
				const bool elem_basic = db.IsBasic(f->type_name);
				if (f->IsArray())
				{
					const u32 stride = db.InlineStride(f->type_name);
					line += fmt::format("inline array of {} at #x{:x} (stride #x{:x})", f->array_size, faddr, stride);
					out += line + '\n';
					if (depth > 0)
					{
						for (u32 i = 0; i < std::min<u32>(f->array_size, 8); i++)
						{
							out += fmt::format("    [{}]\n", i);
							out += Indent(Dump(faddr + i * stride + (elem_basic ? 4 : 0), f->type_name, depth - 1), "      ");
						}
					}
				}
				else
				{
					const u32 obj = faddr + (elem_basic ? 4 : 0);
					if (depth > 0)
					{
						out += line + '\n';
						out += Indent(Dump(obj, f->type_name, depth - 1), "      ");
					}
					else
					{
						out += line + FormatTypedValue(f->type, obj) + " (inline)\n";
					}
				}
				continue;
			}

			const TypeInfo info = db.Info(f->type);
			const u32 elem_size = (info.category == TypeInfo::Category::Reference || info.category == TypeInfo::Category::Unknown) ? 4 : info.size;
			if (f->IsArray())
			{
				line += '[';
				for (u32 i = 0; i < std::min<u32>(f->array_size, MAX_ARRAY_PRINT); i++)
				{
					const u64 v = LoadTypedValue(f->type, faddr + i * elem_size, elem_size, info.is_signed);
					line += (i ? " " : "") + FormatTypedValue(f->type, v, faddr + i * elem_size);
				}
				if (static_cast<u32>(f->array_size) > MAX_ARRAY_PRINT)
					line += " ...";
				line += ']';
			}
			else
			{
				const u64 v = LoadTypedValue(f->type, faddr, elem_size, info.is_signed);
				line += FormatTypedValue(f->type, v, faddr);
			}
			out += line + '\n';
		}

		// show what nobody has claimed yet, it's usually what still needs decompiling
		for (u32 start = 0; start < covered.size();)
		{
			if (covered[start])
			{
				start++;
				continue;
			}
			u32 end = start;
			while (end < covered.size() && !covered[end])
				end++;
			if (end - start >= 4)
			{
				out += fmt::format("  gap +{:#x}..+{:#x} ({} bytes):\n", start, end, end - start);
				for (u32 off = (start + 3) & ~3u; off + 4 <= end; off += 4)
				{
					const u32 w = Mem::Read32(base + off);
					out += fmt::format("    +{:#05x} {:08x}  {}\n", off, w, Guessed(w, Mem::Read32(base + off + 4), (off & 7) == 0));
				}
			}
			start = end;
		}

		return out;
	}

	std::string Guess(u32 addr, u32 size, std::string_view type)
	{
		Runtime& rt = GetRuntime();
		const std::string type_name = ResolveTypeName(addr, type);
		const u32 base = type_name.empty() ? addr : ObjectBase(addr, type_name);
		if (size == 0)
		{
			size = 0x40;
			if (const u32 rtype = rt.BasicType(addr))
				size = std::max<u32>(rt.TypeSize(rtype), 8);
			else if (const u32 dbsize = GetTypeDb().Size(type_name))
				size = dbsize;
		}

		std::string out = fmt::format("[{:08x}] {} ({} bytes from #x{:x})\n", addr, type_name.empty() ? "?" : type_name, size, base);
		for (u32 off = 0; off < size; off += 4)
		{
			const u32 w = Mem::Read32(base + off);
			const std::string field = FieldLabel(type_name, off);
			out += fmt::format("  +{:#05x} {:08x}  {:<44} {}\n", off, w, Guessed(w, Mem::Read32(base + off + 4), (off & 7) == 0),
				field.empty() ? "" : "; " + field);
		}
		return out;
	}

	std::string Hexdump(u32 addr, u32 size)
	{
		std::string out;
		for (u32 line = 0; line < size; line += 16)
		{
			out += fmt::format("{:08x}: ", addr + line);
			std::string ascii;
			for (u32 i = 0; i < 16; i += 4)
			{
				const u32 w = Mem::Read32(addr + line + i);
				out += fmt::format("{:08x} ", w);
				for (u32 b = 0; b < 4; b++)
				{
					const char c = static_cast<char>((w >> (b * 8)) & 0xff);
					ascii += (c >= 0x20 && c < 0x7f) ? c : '.';
				}
			}
			out += ' ' + ascii + '\n';
		}
		return out;
	}

	std::string Xref(u32 addr, u32 size, u32 max_results)
	{
		Runtime& rt = GetRuntime();
		const std::span<const u8> ram = Mem::Ram();
		std::string out;
		u32 found = 0;
		for (u32 a = 0; a + 4 <= ram.size(); a += 4)
		{
			u32 w;
			std::memcpy(&w, &ram[a], 4);
			if (w < addr || w - addr >= size)
				continue;
			if (++found > max_results)
				break;

			std::string where;
			if (a >= rt.SymbolTableStart() && a < rt.SymbolTableEnd())
			{
				const u32 sym = a - static_cast<u32>(rt.GetLayout().symbol_value_off);
				where = rt.IsSymbol(sym) ? fmt::format("value of symbol '{}", rt.SymbolName(sym)) : "symbol table";
			}
			else
			{
				// walk back to the enclosing basic
				for (u32 tag = a & ~7u; tag + 0x8000 > a && tag >= 8; tag -= 8)
				{
					const u32 t = Mem::Read32(tag);
					if (!rt.IsType(t))
						continue;
					const u32 obj = tag + 4;
					u32 extent = rt.TypeSize(t);
					if (rt.TypeInherits(t, "process"))
						extent = std::max<u32>(extent, 0x8000);
					if (a - tag < std::max<u32>(extent, 4))
					{
						const std::string field = FieldLabel(rt.TypeName(t), a - tag);
						where = fmt::format("{} +{:#x}{}", rt.Describe(obj), a - tag, field.empty() ? "" : " (" + field + ")");
					}
					break;
				}
				if (where.empty())
				{
					where = rt.DescribeAddress(a);
					if (where.empty())
						where = "?";
				}
			}
			out += fmt::format("  #x{:08x} -> #x{:x}{}  {}\n", a, w, w == addr ? "" : fmt::format(" (+{:#x})", w - addr), where);
		}
		if (found == 0)
			return fmt::format("nothing in ram points into [#x{:x}, #x{:x})\n", addr, addr + size);
		if (found > max_results)
			out += fmt::format("  ... stopped after {} results\n", max_results);
		return out;
	}

	std::vector<u32> FindInstances(u32 type, u32 max_results)
	{
		Runtime& rt = GetRuntime();
		std::vector<u32> results;
		const std::span<const u8> ram = Mem::Ram();
		for (u32 a = 0x100000; a + 4 <= ram.size(); a += 16)
		{
			u32 w;
			std::memcpy(&w, &ram[a], 4);
			if (w != type)
				continue;
			if (a >= rt.SymbolTableStart() && a < rt.SymbolTableEnd())
				continue;
			results.push_back(a + 4);
			if (results.size() >= max_results)
				break;
		}
		return results;
	}

	std::string Instances(std::string_view type_name, u32 max_results)
	{
		Runtime& rt = GetRuntime();
		const std::optional<u32> type = rt.FindType(type_name);
		if (!type.has_value())
			return fmt::format("no runtime type named {}\n", type_name);
		const std::vector<u32> found = FindInstances(*type, max_results);
		std::string out = fmt::format("{} instance(s) of {} (16 byte aligned basics in main ram):\n", found.size(), type_name);
		for (const u32 obj : found)
		{
			const std::string heap = rt.HeapName(obj);
			out += fmt::format("  {}{}\n", rt.Describe(obj), heap.empty() ? "" : " [" + heap + "]");
		}
		return out;
	}

	std::string FieldStats(std::string_view type_name, u32 max_instances)
	{
		Runtime& rt = GetRuntime();
		const TypeDb& db = GetTypeDb();
		const std::optional<u32> type = rt.FindType(type_name);
		if (!type.has_value())
			return fmt::format("no runtime type named {}\n", type_name);

		const std::vector<u32> instances = FindInstances(*type, max_instances);
		if (instances.empty())
			return fmt::format("no instances of {} found\n", type_name);

		const u32 size = std::max<u32>(rt.TypeSize(*type), db.Size(type_name));
		std::string out = fmt::format("{} over {} instance(s), {} bytes each (tag-relative offsets):\n", type_name, instances.size(), size);
		for (u32 off = 4; off < size; off += 4)
		{
			WordStats stats;
			for (const u32 obj : instances)
				stats.Add(Mem::Read32(obj - 4 + off));

			const FieldDef* f = FieldAt(type_name, off);
			std::string label = "?";
			std::string suggestion;
			if (f)
			{
				const u32 delta = off - static_cast<u32>(f->offset);
				label = delta ? fmt::format("{}+{}", f->name, delta) : f->name;
				const TypeInfo info = db.Info(f->type);
				if (delta == 0 && !f->IsArray() && !f->is_inline && (info.size == 4 || info.category == TypeInfo::Category::Reference))
					suggestion = stats.Suggestion(f->type_name);
			}
			else
			{
				suggestion = stats.Suggestion("uint32");
			}

			out += fmt::format("  +{:#05x} {:<26} {}{}\n", off, Truncate(label, 26), stats.Summary(),
				suggestion.empty() ? "" : fmt::format("   => {}?", suggestion));
		}
		return out;
	}

	std::string TypeReport(std::string_view type_name)
	{
		Runtime& rt = GetRuntime();
		const TypeDb& db = GetTypeDb();
		std::string out;

		const std::optional<u32> type = rt.FindType(type_name);
		if (type.has_value())
		{
			std::string chain;
			for (const u32 t : rt.TypeChain(*type))
				chain += fmt::format("{}{}", chain.empty() ? "" : " -> ", rt.TypeName(t));
			out += fmt::format("runtime: type object #x{:x}\n  parents: {}\n  size #x{:x}, heap-base #x{:x}, {} methods\n", *type, chain,
				rt.TypeSize(*type), rt.TypeHeapBase(*type), rt.TypeMethodCount(*type));
		}
		else
		{
			out += "runtime: not loaded (or not a type)\n";
		}

		const TypeDef* def = db.FindType(type_name);
		if (def)
		{
			std::string chain;
			for (const std::string& t : db.ParentChain(type_name))
				chain += fmt::format("{}{}", chain.empty() ? "" : " -> ", t);
			out += fmt::format("all-types: {}\n  parents: {}\n  size #x{:x}, {} methods, {} own fields{}\n", def->declared_only ? "declare-type only" : "deftype",
				chain, def->size, def->method_count, def->fields.size(), def->pack_me ? ", :pack-me" : "");
			if (!def->doc.empty())
				out += Indent(def->doc, "  | ");
		}
		else
		{
			out += "all-types: not defined\n";
		}

		if (type.has_value() && def && !def->declared_only)
		{
			if (rt.TypeName(rt.TypeParent(*type)) != def->parent)
				out += fmt::format("!! parent differs: runtime {}, all-types {}\n", rt.TypeName(rt.TypeParent(*type)), def->parent);
			if (rt.TypeSize(*type) != def->size)
				out += fmt::format("!! size differs: runtime #x{:x}, all-types #x{:x}\n", rt.TypeSize(*type), def->size);
			if (rt.TypeMethodCount(*type) != def->method_count)
				out += fmt::format("!! method count differs: runtime {}, all-types {}\n", rt.TypeMethodCount(*type), def->method_count);
		}
		return out;
	}

	std::string Methods(std::string_view type_name)
	{
		Runtime& rt = GetRuntime();
		const TypeDb& db = GetTypeDb();
		const std::optional<u32> type = rt.FindType(type_name);
		if (!type.has_value())
			return fmt::format("no runtime type named {}\n", type_name);
		if (rt.CodeMapEmpty())
			rt.RebuildCodeMap();

		const std::optional<u32> state_type = rt.FindType("state");
		std::string out;
		const u32 count = rt.TypeMethodCount(*type);
		for (u32 i = 0; i < count; i++)
		{
			const u32 m = rt.TypeMethod(*type, i);
			const MethodDef* def = db.FindMethod(type_name, static_cast<s32>(i));
			std::string impl;
			if (state_type.has_value() && rt.BasicType(m) == *state_type)
				impl = fmt::format("state {}", rt.Describe(m));
			else if (const CodeLabel* label = rt.LookupCode(m))
				impl = rt.CodeLabelName(*label);
			else
				impl = rt.Describe(m);
			const bool inherited = rt.TypeMethodCount(rt.TypeParent(*type)) > i && rt.TypeMethod(rt.TypeParent(*type), i) == m;
			out += fmt::format("  {:3} #x{:08x} {:<36} {:<28} {}\n", i, m, Truncate(impl, 36), def ? def->name : "?",
				inherited ? "(inherited)" : (def ? def->Signature() : ""));
		}
		return out;
	}

	std::string TypeCheck(std::string_view filter)
	{
		Runtime& rt = GetRuntime();
		const TypeDb& db = GetTypeDb();
		if (!db.IsLoaded())
			return "load all-types.gc first: types <path>\n";

		std::string out;
		u32 checked = 0, missing = 0, mismatched = 0;
		std::vector<std::string> missing_names;
		rt.ForEachSymbol([&](u32 sym, const std::string& name) {
			const u32 value = rt.SymbolValue(sym);
			if (!rt.IsType(value) || rt.TypeName(value) != name)
				return;
			if (!filter.empty() && name.find(filter) == std::string::npos)
				return;
			checked++;
			const TypeDef* def = db.FindType(name);
			if (!def || def->declared_only)
			{
				missing++;
				missing_names.push_back(fmt::format("{} (#x{:x} bytes, parent {})", name, rt.TypeSize(value), rt.TypeName(rt.TypeParent(value))));
				return;
			}
			if (def->builtin)
				return;
			std::string problems;
			const std::string parent = rt.TypeName(rt.TypeParent(value));
			if (parent != def->parent)
				problems += fmt::format(" parent {} vs {};", parent, def->parent);
			if (rt.TypeSize(value) != def->size)
				problems += fmt::format(" size #x{:x} vs #x{:x};", rt.TypeSize(value), def->size);
			if (rt.TypeMethodCount(value) != def->method_count)
				problems += fmt::format(" methods {} vs {};", rt.TypeMethodCount(value), def->method_count);
			if (!problems.empty())
			{
				mismatched++;
				out += fmt::format("  {:<36} runtime vs all-types:{}\n", name, problems);
			}
		});
		std::sort(missing_names.begin(), missing_names.end());
		if (!missing_names.empty())
		{
			out += fmt::format("types loaded in the game but missing from all-types ({}):\n", missing);
			for (const std::string& m : missing_names)
				out += "  " + m + '\n';
		}
		out += fmt::format("{} runtime types checked, {} mismatched, {} missing\n", checked, mismatched, missing);
		return out;
	}

	namespace
	{
		struct TreeOffsets
		{
			u32 name = 4, brother = 24, child = 28;
			std::optional<u32> state;
			std::optional<u32> state_name;
		};

		TreeOffsets GetTreeOffsets()
		{
			const TypeDb& db = GetTypeDb();
			TreeOffsets o;
			if (const FieldDef* f = db.FindField("process-tree", "name"))
				o.name = f->offset;
			if (const FieldDef* f = db.FindField("process-tree", "brother"))
				o.brother = f->offset;
			if (const FieldDef* f = db.FindField("process-tree", "child"))
				o.child = f->offset;
			if (const FieldDef* f = db.FindField("process", "state"))
				o.state = f->offset;
			if (const FieldDef* f = db.FindField("state", "name"))
				o.state_name = f->offset;
			return o;
		}

		void WalkTree(u32 node, u32 depth, const TreeOffsets& o, std::unordered_set<u32>& seen,
			const std::function<void(u32, u32)>& visit)
		{
			Runtime& rt = GetRuntime();
			while (node && rt.IsBasic(node) && seen.size() < 8192 && seen.insert(node).second)
			{
				visit(node, depth);
				const u32 child_ptr = Mem::Read32(node - 4 + o.child);
				if (child_ptr && Mem::IsRam(child_ptr))
					WalkTree(Mem::Read32(child_ptr), depth + 1, o, seen, visit);
				const u32 brother_ptr = Mem::Read32(node - 4 + o.brother);
				node = (brother_ptr && Mem::IsRam(brother_ptr)) ? Mem::Read32(brother_ptr) : 0;
			}
		}

		std::string ProcessName(u32 node, const TreeOffsets& o)
		{
			Runtime& rt = GetRuntime();
			const u32 name = Mem::Read32(node - 4 + o.name);
			return rt.IsString(name) ? rt.StringValue(name, 64) : std::string();
		}
	} // namespace

	std::string Processes(std::string_view filter)
	{
		Runtime& rt = GetRuntime();
		const std::optional<u32> pool_sym = rt.FindSymbol("*active-pool*");
		if (!pool_sym.has_value())
			return "*active-pool* is not defined yet\n";

		const TreeOffsets o = GetTreeOffsets();
		std::unordered_set<u32> seen;
		std::string out;
		u32 count = 0;
		WalkTree(rt.SymbolValue(*pool_sym), 0, o, seen, [&](u32 node, u32 depth) {
			const std::string name = ProcessName(node, o);
			const std::string type = rt.TypeName(rt.BasicType(node));
			if (!filter.empty() && name.find(filter) == std::string::npos && type.find(filter) == std::string::npos)
				return;
			std::string line = fmt::format("{:{}}{} \"{}\" @ #x{:x}", "", depth * 2, type, name, node);
			if (o.state.has_value() && rt.TypeInherits(rt.BasicType(node), "process"))
			{
				const u32 state = Mem::Read32(node - 4 + *o.state);
				if (rt.IsBasic(state) && o.state_name.has_value())
					line += fmt::format("  [{}]", rt.Describe(Mem::Read32(state - 4 + *o.state_name)));
			}
			out += line + '\n';
			count++;
		});
		out += fmt::format("{} node(s)\n", count);
		return out;
	}

	std::optional<u32> FindProcess(std::string_view name)
	{
		Runtime& rt = GetRuntime();
		const std::optional<u32> pool_sym = rt.FindSymbol("*active-pool*");
		if (!pool_sym.has_value())
			return std::nullopt;
		const TreeOffsets o = GetTreeOffsets();
		std::unordered_set<u32> seen;
		std::optional<u32> found;
		WalkTree(rt.SymbolValue(*pool_sym), 0, o, seen, [&](u32 node, u32) {
			if (!found.has_value() && ProcessName(node, o) == name)
				found = node;
		});
		return found;
	}

	std::string Apropos(std::string_view needle, u32 max_results)
	{
		Runtime& rt = GetRuntime();
		std::vector<std::string> lines;
		rt.ForEachSymbol([&](u32 sym, const std::string& name) {
			if (name.find(needle) == std::string::npos)
				return;
			lines.push_back(fmt::format("  {:<40} {}", name, Truncate(rt.Describe(rt.SymbolValue(sym)), 70)));
		});
		std::sort(lines.begin(), lines.end());
		std::string out;
		for (size_t i = 0; i < lines.size() && i < max_results; i++)
			out += lines[i] + '\n';
		out += fmt::format("{} match(es){}\n", lines.size(), lines.size() > max_results ? fmt::format(", showing {}", max_results) : "");
		return out;
	}

	std::string SymbolReport(std::string_view name)
	{
		Runtime& rt = GetRuntime();
		const std::optional<u32> sym = rt.FindSymbol(name);
		if (!sym.has_value())
			return fmt::format("no symbol named {}\n", name);
		const u32 value = rt.SymbolValue(*sym);
		std::string out = fmt::format("symbol '{} @ #x{:x}, value slot #x{:x}\n  value: {}\n", name, *sym, rt.SymbolValueAddr(*sym),
			rt.Describe(value, true));
		if (const SExpr* spec = GetTypeDb().FindGlobal(name))
			out += fmt::format("  all-types: (define-extern {} {})\n", name, spec->Print());
		return out;
	}

	std::string Snapshot(const std::string& name, u32 addr, u32 size, std::string_view type)
	{
		std::vector<u8> bytes(size);
		Mem::Read(addr, bytes.data(), size);
		s_snapshots[name] = {addr, std::string(type), std::move(bytes)};
		return fmt::format("saved {} bytes at #x{:x} as '{}'\n", size, addr, name);
	}

	std::string Diff(const std::string& name)
	{
		const auto it = s_snapshots.find(name);
		if (it == s_snapshots.end())
			return fmt::format("no snapshot named '{}'\n", name);
		Runtime& rt = GetRuntime();
		auto& [addr, static_type, old_bytes] = it->second;
		const std::string type_name = ResolveTypeName(addr, static_type);
		const u32 base = type_name.empty() ? addr : ObjectBase(addr, type_name);
		const u32 delta = addr - base;

		std::vector<u8> now(old_bytes.size());
		Mem::Read(addr, now.data(), static_cast<u32>(now.size()));
		std::string out;
		u32 changed = 0;
		for (u32 off = 0; off + 4 <= now.size(); off += 4)
		{
			u32 a, b;
			std::memcpy(&a, &old_bytes[off], 4);
			std::memcpy(&b, &now[off], 4);
			if (a == b)
				continue;
			changed++;
			const std::string field = FieldLabel(type_name, off + delta);
			out += fmt::format("  +{:#05x} {:<30} {} -> {}\n", off + delta, Truncate(field.empty() ? "?" : field, 30), rt.Describe(a, false),
				rt.Describe(b, false));
		}
		old_bytes = std::move(now);
		out += fmt::format("{} word(s) changed since the last snapshot/diff\n", changed);
		return out;
	}

	std::string ListSnapshots()
	{
		std::string out;
		for (const auto& [name, snap] : s_snapshots)
			out += fmt::format("  {:<16} #x{:x} {} bytes {}\n", name, std::get<0>(snap), std::get<2>(snap).size(), std::get<1>(snap));
		return out.empty() ? "no snapshots\n" : out;
	}
} // namespace Goal::Inspector
