// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalRepl.h"
#include "GoalCaller.h"
#include "GoalInspector.h"
#include "GoalRuntime.h"
#include "GoalTypeDb.h"
#include "JakGoalDebugInspect.h"

#include "Host.h"
#include "VMManager.h"

#include "common/StringUtil.h"

#include "fmt/format.h"

#include <bit>
#include <charconv>
#include <cstdlib>

namespace Goal::Repl
{
	namespace
	{
		struct Value
		{
			u64 bits = 0;
			std::optional<SExpr> type;
		};

		SExpr Atom(std::string_view text)
		{
			SExpr e;
			e.kind = SExpr::Kind::Atom;
			e.text = std::string(text);
			return e;
		}

		SExpr List(std::initializer_list<SExpr> items)
		{
			SExpr e;
			e.kind = SExpr::Kind::List;
			e.items = items;
			return e;
		}

		std::string Head(const SExpr& type)
		{
			if (type.IsList())
				return type.items.empty() ? std::string() : type.items[0].text;
			return type.text;
		}

		std::optional<SExpr> ParseTypeSpec(std::string_view text)
		{
			std::vector<SExpr> forms;
			std::string error;
			if (!ParseSExprs(text, &forms, &error) || forms.size() != 1)
				return std::nullopt;
			return forms[0];
		}

		std::optional<float> ParseFloat(std::string_view text)
		{
			if (text.find('.') == std::string_view::npos)
				return std::nullopt;
			if (text.ends_with('f'))
				text.remove_suffix(1);
			float value = 0.0f;
			const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (ec != std::errc() || ptr != text.data() + text.size())
				return std::nullopt;
			return value;
		}

		std::string ExpandPath(std::string path)
		{
			if (path.starts_with("~/"))
			{
				if (const char* home = std::getenv("HOME"))
					path = std::string(home) + path.substr(1);
			}
			return path;
		}

		class Evaluator
		{
		public:
			std::string output;
			std::string error;

			std::optional<Value> Eval(const SExpr& e)
			{
				if (e.IsString())
				{
					const std::optional<u32> addr = GetCaller().AllocTempString(e.text);
					if (!addr.has_value())
						return Fail("no room for temporary strings");
					return Value{*addr, Atom("string")};
				}
				if (e.IsAtom())
					return EvalAtom(e.text);
				if (e.items.empty())
					return Value{GetRuntime().EmptyPair(), Atom("pair")};
				return EvalList(e);
			}

			// static type of a value, refined to the runtime type when it is a more derived basic
			std::string TypeOf(const Value& v)
			{
				Runtime& rt = GetRuntime();
				const TypeDb& db = GetTypeDb();
				const std::string declared = v.type.has_value() ? Head(*v.type) : std::string();
				if (declared == "pointer" || declared == "inline-array")
					return declared;
				const u32 runtime_type = rt.BasicType(static_cast<u32>(v.bits));
				if (runtime_type && (declared.empty() || db.IsBasic(declared) || !db.FindType(declared)))
				{
					const std::string actual = rt.TypeName(runtime_type);
					if (declared.empty() || db.Inherits(actual, declared) || !db.FindType(declared))
						return actual;
				}
				return declared;
			}

			std::optional<Value> Call(u32 fn, const std::vector<Value>& args, std::optional<SExpr> ret_type)
			{
				std::vector<u64> raw;
				for (const Value& a : args)
					raw.push_back(a.bits);
				const CallResult result = GetCaller().Call(fn, raw);
				output += result.output;
				if (!result.output.empty() && !result.output.ends_with('\n'))
					output += '\n';
				for (const std::string& note : result.notes)
					output += "; note: " + note + '\n';
				if (!result.ok)
					return Fail(result.error);
				return Value{result.ret, std::move(ret_type)};
			}

		private:
			std::optional<Value> Fail(std::string message)
			{
				if (error.empty())
					error = std::move(message);
				return std::nullopt;
			}

			std::optional<Value> EvalAtom(const std::string& text)
			{
				Runtime& rt = GetRuntime();
				if (const std::optional<s64> n = ParseInteger(text))
					return Value{static_cast<u64>(*n), std::nullopt};
				if (const std::optional<float> f = ParseFloat(text))
					return Value{std::bit_cast<u32>(*f), Atom("float")};
				if (text == "#t")
					return Value{rt.TrueSym(), Atom("symbol")};
				if (text == "#f")
					return Value{rt.FalseSym(), Atom("symbol")};
				if (text == "'()")
					return Value{rt.EmptyPair(), Atom("pair")};
				if (text.starts_with('\''))
				{
					const std::optional<u32> sym = rt.FindSymbol(std::string_view(text).substr(1));
					if (!sym.has_value())
						return Fail(fmt::format("no symbol named {}", text.substr(1)));
					return Value{*sym, Atom("symbol")};
				}

				const std::optional<u32> sym = rt.FindSymbol(text);
				if (!sym.has_value())
					return Fail(fmt::format("no symbol named {}", text));
				Value v{rt.SymbolValue(*sym), std::nullopt};
				if (const SExpr* spec = GetTypeDb().FindGlobal(text))
					v.type = *spec;
				return v;
			}

			std::optional<s32> MethodId(std::string_view type, const SExpr& method)
			{
				if (method.IsAtom())
				{
					if (const std::optional<s64> n = ParseInteger(method.text))
						return static_cast<s32>(*n);
					static constexpr std::string_view builtin[] = {"new", "delete", "print", "inspect", "length", "asize-of", "copy", "relocate", "mem-usage"};
					for (s32 i = 0; i < static_cast<s32>(std::size(builtin)); i++)
					{
						if (method.text == builtin[i])
							return i;
					}
					if (const MethodDef* m = GetTypeDb().FindMethodByName(type, method.text))
						return m->id;
				}
				Fail(fmt::format("type {} has no method {}", type, method.Print()));
				return std::nullopt;
			}

			std::optional<u32> MethodOf(std::string_view type_name, const SExpr& method, std::optional<SExpr>* ret_type)
			{
				Runtime& rt = GetRuntime();
				const std::optional<u32> type = rt.FindType(type_name);
				if (!type.has_value())
				{
					Fail(fmt::format("no runtime type named {}", type_name));
					return std::nullopt;
				}
				const std::optional<s32> id = MethodId(type_name, method);
				if (!id.has_value())
					return std::nullopt;
				if (*id < 0 || static_cast<u32>(*id) >= rt.TypeMethodCount(*type))
				{
					Fail(fmt::format("{} only has {} methods", type_name, rt.TypeMethodCount(*type)));
					return std::nullopt;
				}
				if (ret_type)
				{
					if (const MethodDef* m = GetTypeDb().FindMethod(type_name, *id))
					{
						*ret_type = (m->ret == "_type_") ? std::optional<SExpr>(Atom(type_name)) : ParseTypeSpec(m->ret);
						if (m->ret == "none")
							ret_type->reset();
					}
				}
				return rt.TypeMethod(*type, *id);
			}

			// one step of (-> obj ...)
			std::optional<Value> Step(const Value& obj, const SExpr& key, bool address_only)
			{
				Runtime& rt = GetRuntime();
				const TypeDb& db = GetTypeDb();
				const std::string type = TypeOf(obj);

				// indexing into pointers, inline arrays and runtime arrays
				std::optional<s64> index;
				if (key.IsAtom())
					index = ParseInteger(key.text);
				else
				{
					Evaluator sub;
					const std::optional<Value> k = sub.Eval(key);
					output += sub.output;
					if (!k.has_value())
						return Fail(sub.error);
					index = static_cast<s64>(k->bits);
				}

				if (index.has_value())
				{
					const u32 base = static_cast<u32>(obj.bits);
					if ((type == "pointer" || type == "inline-array") && obj.type.has_value() && obj.type->IsList() && obj.type->items.size() > 1)
					{
						const SExpr elem = obj.type->items[1];
						if (type == "inline-array")
						{
							const u32 addr = base + static_cast<u32>(*index) * db.InlineStride(Head(elem)) + (db.IsBasic(Head(elem)) ? 4 : 0);
							return Value{addr, elem};
						}
						const TypeInfo info = db.Info(elem);
						const u32 size = (info.category == TypeInfo::Category::Reference || info.category == TypeInfo::Category::Unknown) ? 4 : info.size;
						const u32 addr = base + static_cast<u32>(*index) * size;
						if (address_only)
							return Value{addr, List({Atom("pointer"), elem})};
						return Value{Inspector::LoadTypedValue(elem, addr, size, info.is_signed), elem};
					}
					if (rt.TypeInherits(rt.BasicType(base), "array"))
					{
						const u32 content = Mem::Read32(base + 8);
						const std::string content_name = rt.TypeName(content);
						const TypeInfo info = db.Info(content_name);
						const u32 size = (info.category == TypeInfo::Category::Reference || info.category == TypeInfo::Category::Unknown) ? 4 : info.size;
						const u32 addr = base + 12 + static_cast<u32>(*index) * size;
						if (address_only)
							return Value{addr, List({Atom("pointer"), Atom(content_name)})};
						return Value{Inspector::LoadTypedValue(Atom(content_name), addr, size, info.is_signed), Atom(content_name)};
					}
					return Fail(fmt::format("can't index into a {}", type.empty() ? "value of unknown type" : type));
				}

				if (type.empty())
					return Fail(fmt::format("#x{:x} has no known type, use (the <type> ...)", obj.bits));
				const FieldDef* f = db.FindField(type, key.text);
				if (!f)
					return Fail(fmt::format("type {} has no field {}", type, key.text));

				// fields of bitfield value types are bit ranges
				if (db.Info(type).category == TypeInfo::Category::Bitfield)
				{
					const s32 bits = f->bit_size > 0 ? f->bit_size : static_cast<s32>(db.Info(f->type).size * 8);
					const u64 mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
					return Value{(obj.bits >> f->offset) & mask, f->type};
				}

				const u32 faddr = Inspector::ObjectBase(static_cast<u32>(obj.bits), type) + static_cast<u32>(f->offset);
				if (f->is_inline)
				{
					if (f->IsArray())
						return Value{faddr, List({Atom("inline-array"), f->type})};
					return Value{faddr + (db.IsBasic(f->type_name) ? 4u : 0u), f->type};
				}
				if (f->IsArray() || f->is_dynamic)
					return Value{faddr, List({Atom("pointer"), f->type})};
				if (address_only)
					return Value{faddr, List({Atom("pointer"), f->type})};

				const TypeInfo info = db.Info(f->type);
				const u32 size = (info.category == TypeInfo::Category::Reference || info.category == TypeInfo::Category::Unknown) ? 4 : info.size;
				return Value{Inspector::LoadTypedValue(f->type, faddr, size, info.is_signed), f->type};
			}

			std::optional<Value> InspectOrPrint(const SExpr& e, u32 method)
			{
				Runtime& rt = GetRuntime();
				const TypeDb& db = GetTypeDb();
				if (e.items.size() < 2)
					return Fail("missing argument");
				const std::optional<Value> v = Eval(e.items[1]);
				if (!v.has_value())
					return std::nullopt;

				std::string type_name;
				if (e.items.size() > 2)
					type_name = e.items[2].text;
				else if (v->type.has_value() && db.FindType(Head(*v->type)) && !db.IsBasic(Head(*v->type)) && db.IsStructure(Head(*v->type)))
					type_name = Head(*v->type);

				if (method == 3)
				{
					if (std::optional<std::string> dump = NativeInspect(*v, type_name))
					{
						output += *dump;
						return v;
					}
				}

				if (!type_name.empty())
				{
					// structures carry no type tag, so call the method of the type we know statically
					const std::optional<u32> fn = MethodOf(type_name, Atom(std::to_string(method)), nullptr);
					if (!fn.has_value())
						return std::nullopt;
					return Call(*fn, {*v}, v->type);
				}

				const std::optional<u32> sym = rt.FindSymbol(method == 3 ? "inspect" : "print");
				if (!sym.has_value() || !rt.IsFunction(rt.SymbolValue(*sym)))
					return Fail("the game has no inspect/print function yet");
				return Call(rt.SymbolValue(*sym), {*v}, v->type);
			}

			// retail builds don't load the debug segment, which holds nearly every inspect method.
			// types without their own inspect would only print their parent's fields, so dump them natively
			std::optional<std::string> NativeInspect(const Value& v, const std::string& static_type)
			{
				Runtime& rt = GetRuntime();
				const TypeDb& db = GetTypeDb();
				const std::string name = static_type.empty() ? TypeOf(v) : static_type;
				const std::optional<u32> type = name.empty() ? std::nullopt : rt.FindType(name);
				if (!type.has_value() || !db.FindType(name))
					return std::nullopt;
				const u32 parent = rt.TypeParent(*type);
				if (!rt.IsType(parent) || rt.TypeMethod(*type, 3) != rt.TypeMethod(parent, 3))
					return std::nullopt;
				return fmt::format("; note: {} has no inspect method of its own in memory (debug segment not loaded?), dumping with all-types\n{}",
					name, Inspector::Dump(static_cast<u32>(v.bits), name, 1));
			}

			std::optional<Value> EvalList(const SExpr& e)
			{
				Runtime& rt = GetRuntime();
				const TypeDb& db = GetTypeDb();
				const std::string head = e.items[0].IsAtom() ? e.items[0].text : std::string();

				if (head == "->" || head == "&->")
				{
					if (e.items.size() < 3)
						return Fail("(-> obj field...)");
					std::optional<Value> v = Eval(e.items[1]);
					for (size_t i = 2; v.has_value() && i < e.items.size(); i++)
						v = Step(*v, e.items[i], head == "&->" && i + 1 == e.items.size());
					return v;
				}
				if (head == "the" || head == "the-as")
				{
					if (e.items.size() != 3)
						return Fail("(the <type> <expr>)");
					std::optional<Value> v = Eval(e.items[2]);
					if (v.has_value())
						v->type = e.items[1];
					return v;
				}
				if (head == "+" || head == "-" || head == "*" || head == "logand" || head == "logior")
				{
					std::optional<Value> acc;
					for (size_t i = 1; i < e.items.size(); i++)
					{
						const std::optional<Value> v = Eval(e.items[i]);
						if (!v.has_value())
							return std::nullopt;
						if (!acc.has_value())
						{
							acc = Value{v->bits, std::nullopt};
							continue;
						}
						if (head == "+")
							acc->bits += v->bits;
						else if (head == "-")
							acc->bits -= v->bits;
						else if (head == "*")
							acc->bits *= v->bits;
						else if (head == "logand")
							acc->bits &= v->bits;
						else
							acc->bits |= v->bits;
					}
					if (!acc.has_value())
						return Fail("no operands");
					return acc;
				}
				if (head == "inspect")
					return InspectOrPrint(e, 3);
				if (head == "print")
					return InspectOrPrint(e, 2);
				if (head == "type-of")
				{
					if (e.items.size() != 2)
						return Fail("(type-of obj)");
					const std::optional<Value> v = Eval(e.items[1]);
					if (!v.has_value())
						return std::nullopt;
					return Value{rt.DynamicTypeOf(v->bits), Atom("type")};
				}
				if (head == "ppointer->process")
				{
					if (e.items.size() != 2)
						return Fail("(ppointer->process ptr)");
					const std::optional<Value> v = Eval(e.items[1]);
					if (!v.has_value())
						return std::nullopt;
					return Value{Mem::Read32(static_cast<u32>(v->bits)), Atom("process")};
				}
				if (head == "handle->process")
				{
					if (e.items.size() != 2)
						return Fail("(handle->process handle)");
					const std::optional<Value> v = Eval(e.items[1]);
					if (!v.has_value())
						return std::nullopt;
					const u32 ppointer = static_cast<u32>(v->bits);
					const u32 proc = Mem::IsRam(ppointer) && ppointer ? Mem::Read32(ppointer) : 0;
					if (const FieldDef* pid = db.FindField("process", "pid"); proc && pid &&
																			 Mem::Read32(proc - 4 + pid->offset) != static_cast<u32>(v->bits >> 32))
						return Value{rt.FalseSym(), Atom("symbol")};
					return Value{proc ? proc : rt.FalseSym(), Atom("process")};
				}
				if (head == "process-by-name")
				{
					if (e.items.size() != 2)
						return Fail("(process-by-name \"name\")");
					const std::optional<u32> proc = Inspector::FindProcess(e.items[1].text);
					if (!proc.has_value())
						return Fail(fmt::format("no process named {}", e.items[1].text));
					return Value{*proc, Atom(rt.TypeName(rt.BasicType(*proc)))};
				}
				if (head == "deref")
				{
					if (e.items.size() != 3)
						return Fail("(deref <type> <addr>)");
					const std::optional<Value> v = Eval(e.items[2]);
					if (!v.has_value())
						return std::nullopt;
					const TypeInfo info = db.Info(e.items[1]);
					const u32 size = (info.category == TypeInfo::Category::Reference || info.category == TypeInfo::Category::Unknown) ? 4 : info.size;
					return Value{Inspector::LoadTypedValue(e.items[1], static_cast<u32>(v->bits), size, info.is_signed), e.items[1]};
				}
				if (head == "method-of-type")
				{
					if (e.items.size() != 3)
						return Fail("(method-of-type type method)");
					const std::optional<u32> fn = MethodOf(e.items[1].text, e.items[2], nullptr);
					if (!fn.has_value())
						return std::nullopt;
					return Value{*fn, Atom("function")};
				}
				if (head == "method-of-object" || head == "call-method")
				{
					if (e.items.size() < 3)
						return Fail(fmt::format("({} obj method ...)", head));
					const std::optional<Value> obj = Eval(e.items[1]);
					if (!obj.has_value())
						return std::nullopt;
					const std::string type = TypeOf(*obj);
					if (type.empty())
						return Fail("object has no known type, use (the <type> ...)");
					std::optional<SExpr> ret_type;
					const std::optional<u32> fn = MethodOf(type, e.items[2], &ret_type);
					if (!fn.has_value())
						return std::nullopt;
					if (head == "method-of-object")
						return Value{*fn, Atom("function")};
					std::vector<Value> args = {*obj};
					for (size_t i = 3; i < e.items.size(); i++)
					{
						const std::optional<Value> a = Eval(e.items[i]);
						if (!a.has_value())
							return std::nullopt;
						args.push_back(*a);
					}
					return Call(*fn, args, ret_type);
				}

				// a plain function call
				const std::optional<Value> fn = Eval(e.items[0]);
				if (!fn.has_value())
					return std::nullopt;
				if (!rt.IsFunction(static_cast<u32>(fn->bits)))
					return Fail(fmt::format("{} is not a function: {}", e.items[0].Print(), rt.Describe(fn->bits)));

				std::optional<SExpr> ret_type;
				if (fn->type.has_value())
				{
					if (const std::optional<FunctionSig> sig = db.FunctionSignature(*fn->type); sig.has_value() && !sig->ret.IsAtom("none"))
						ret_type = sig->ret;
				}

				std::vector<Value> args;
				for (size_t i = 1; i < e.items.size(); i++)
				{
					const std::optional<Value> a = Eval(e.items[i]);
					if (!a.has_value())
						return std::nullopt;
					args.push_back(*a);
				}
				return Call(static_cast<u32>(fn->bits), args, ret_type);
			}
		};

		std::string FormatValue(const Value& v)
		{
			if (v.type.has_value())
				return fmt::format("{}  ; {}", Inspector::FormatTypedValue(*v.type, v.bits), v.type->Print());
			return GetRuntime().Describe(v.bits, true);
		}

		constexpr const char* HELP = R"(GOAL debugger REPL. Expressions use GOAL syntax, anything else is a command.

expressions
  *target*  'symbol  #t  #f  123  #x10  1.5  "string"
  (-> obj field [index] ...)   read fields using all-types.gc (&-> for the address)
  (the <type> expr)            give a value a type, e.g. (the vector #x123450)
  (inspect obj [type])         run the game's inspect method, output is captured
                               (types whose inspect isn't loaded, e.g. in retail, get a native dump)
  (print obj [type])           run the game's print method
  (some-function args...)      call any GOAL function inside the game
  (call-method obj method args...)   method by name or id; (method-of-type type method)
  (process-by-name "name")  (handle->process h)  (ppointer->process p)  (type-of x)  (deref <type> addr)
  (+ a b) (- a b) (* a b) (logand a b) (logior a b)

inspection (native, no game code runs)
  dump <expr> [type] [depth]   typed dump from all-types, unclaimed bytes are listed as gaps
  guess <expr> [size] [type]   classify every word: symbol / basic / float / pointer / handle...
  mem <expr> [size]            hexdump
  xref <expr> [size] [max]     find everything in ram that points at an object, and who owns it
  instances <type> [max]       find objects of a type
  fieldstats <type> [max]      per-offset value statistics over all instances, with type suggestions
  snap <name> <expr> <size> [type] / diff <name> / snaps   watch what changes
  sym <name>  apropos <text> [max]  procs [filter]
  type <name>  methods <type>  typecheck [filter]   compare runtime types with all-types

call analysis (see what functions get called with)
  analysis on|off              hook every call site (costs performance while on)
  analysis trace <targets...>  hook only these: function name, type (all its methods), type:method, #xaddr
  analysis untrace [targets|all]  analysis traced
  analysis show <target>       observed argument/return types vs all-types
  analysis top [n] | stats | reset | dump [path] | autodump <frames, 0 = off>

setup
  status
  types [path]                 load all-types.gc (remembered in the settings)
  game [jak1|jak2|jak3|jakx|auto]
  scratch [addr|auto]          memory used to run calls (saved and restored around each call)
  codemap                      rebuild the function/method name map
  symbols [reload|<dir>]       symbol names recovered from the disc's DGO/CGO files (retail jak 3 / jak x),
                               reload reads the disc again, <dir> reads extracted DGO/CGO files instead
)";

		std::string Status()
		{
			Runtime& rt = GetRuntime();
			const TypeDb& db = GetTypeDb();
			std::string out;
			rt.Refresh();
			out += fmt::format("game: {}{}\n", GameVersionName(rt.Version()), rt.VersionOverride().has_value() ? " (forced)" : " (from the disc serial)");
			if (rt.IsValid())
			{
				out += fmt::format("runtime: s7 = #x{:x}, symbol table #x{:x}-#x{:x}\n", rt.S7(), rt.SymbolTableStart(), rt.SymbolTableEnd());
				if (rt.UsesRecoveredNames())
					out += SymbolNames::Status();
			}
			else
			{
				out += fmt::format("runtime: {}\n", rt.Error());
			}
			if (db.IsLoaded())
				out += fmt::format("types: {} ({} types, {} enums, {} globals)\n", db.Path(), db.TypeCount(), db.EnumCount(), db.GlobalCount());
			else
				out += "types: not loaded, use 'types <path to all-types.gc>'\n";
			out += GetCaller().Status();
			out += GoalAnalysis::Stats();
			return out;
		}

		std::string SetAnalysisEnabled(bool enabled)
		{
			Host::SetBaseBoolSettingValue("EmuCore", "EnableJakFunctionAnalysis", enabled);
			Host::CommitBaseSettingChanges();
			VMManager::ApplySettings();
			return fmt::format("call site analysis {}\n", enabled ? "enabled, every GOAL call is now hooked" : "disabled");
		}

		std::string Analysis(const std::vector<SExpr>& args)
		{
			const std::string sub = args.empty() ? "stats" : args[0].text;
			std::vector<std::string> rest;
			for (size_t i = 1; i < args.size(); i++)
				rest.push_back(args[i].IsString() ? args[i].text : args[i].Print());

			if (sub == "on")
				return SetAnalysisEnabled(true);
			if (sub == "off")
				return SetAnalysisEnabled(false);
			if (sub == "trace")
				return GoalAnalysis::Trace(rest);
			if (sub == "untrace")
				return GoalAnalysis::Untrace(rest);
			if (sub == "traced")
				return GoalAnalysis::ListTraced();
			if (sub == "show" && !rest.empty())
				return GoalAnalysis::Show(rest[0]);
			if (sub == "top")
				return GoalAnalysis::Top(rest.empty() ? 30 : static_cast<u32>(ParseInteger(rest[0]).value_or(30)));
			if (sub == "reset")
			{
				GoalAnalysis::Reset();
				return "cleared\n";
			}
			if (sub == "dump")
				return GoalAnalysis::WriteJson(rest.empty() ? std::string() : ExpandPath(rest[0]), false);
			if (sub == "autodump" && !rest.empty())
			{
				GoalAnalysis::SetAutoDump(static_cast<u32>(ParseInteger(rest[0]).value_or(0)));
				return "ok\n";
			}
			return GoalAnalysis::Stats();
		}
	} // namespace

	std::string Execute(std::string_view line)
	{
		line = StringUtil::StripWhitespace(line);
		if (line.empty())
			return {};

		Runtime& rt = GetRuntime();
		TypeDb& db = GetTypeDb();

		std::string command;
		std::string_view rest_text;
		if (line[0] == '(' || line[0] == '\'' || line[0] == '*' || line[0] == '#')
		{
			command = "eval";
			rest_text = line;
		}
		else
		{
			const size_t space = line.find_first_of(" \t");
			command = std::string(line.substr(0, space));
			rest_text = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
		}

		std::vector<SExpr> args;
		std::string parse_error;
		if (!ParseSExprs(rest_text, &args, &parse_error))
			return "parse error: " + parse_error + '\n';

		if (command == "help" || command == "?")
			return HELP;
		if (command == "status")
			return Status();
		if (command == "types")
		{
			if (args.empty())
				return db.IsLoaded() ? fmt::format("{} ({} types)\n", db.Path(), db.TypeCount()) : "no all-types.gc loaded\n";
			const std::string path = ExpandPath(args[0].text);
			std::string error;
			if (!db.Load(path, &error))
				return "failed: " + error + '\n';
			Host::SetBaseStringSettingValue("JakDebug", "AllTypesPath", path.c_str());
			Host::CommitBaseSettingChanges();
			rt.InvalidateSymbolCache();
			return fmt::format("loaded {} types, {} enums, {} globals from {}\n", db.TypeCount(), db.EnumCount(), db.GlobalCount(), path);
		}
		if (command == "game")
		{
			if (args.empty() || args[0].text == "auto")
				rt.SetVersionOverride(std::nullopt);
			else if (const std::optional<GameVersion> v = ParseGameVersion(args[0].text))
				rt.SetVersionOverride(*v);
			else
				return "unknown game, use jak1, jak2, jak3, jakx or auto\n";
			GetCaller().ResetCalibration();
			return Status();
		}
		if (command == "scratch")
		{
			if (!args.empty())
			{
				if (args[0].text == "auto")
					GetCaller().SetScratchBase(0);
				else if (const std::optional<s64> addr = ParseInteger(args[0].text))
					GetCaller().SetScratchBase(static_cast<u32>(*addr));
				else
					return "scratch <addr|auto>\n";
			}
			return GetCaller().Status();
		}

		if (!rt.Refresh())
			return rt.Error() + '\n';

		if (command == "analysis")
			return Analysis(args);
		if (command == "symbols")
		{
			if (args.empty())
				return rt.UsesRecoveredNames() ? SymbolNames::Status() : "all symbol names are in memory\n";
			const std::string result = SymbolNames::Reload(args[0].text == "reload" ? std::string() : ExpandPath(args[0].text));
			rt.InvalidateSymbolCache();
			rt.Refresh();
			rt.RebuildCodeMap();
			return result;
		}
		if (command == "codemap")
		{
			rt.RebuildCodeMap();
			return fmt::format("{} functions, methods and state handlers named\n", rt.CodeMapSize());
		}
		if (command == "sym")
			return args.empty() ? "sym <name>\n" : Inspector::SymbolReport(args[0].text);
		if (command == "apropos")
			return args.empty() ? "apropos <text> [max]\n" : Inspector::Apropos(args[0].text, args.size() > 1 ? static_cast<u32>(ParseInteger(args[1].text).value_or(200)) : 200);
		if (command == "procs")
			return Inspector::Processes(args.empty() ? std::string_view() : std::string_view(args[0].text));
		if (command == "type")
			return args.empty() ? "type <name>\n" : Inspector::TypeReport(args[0].text);
		if (command == "methods")
			return args.empty() ? "methods <type>\n" : Inspector::Methods(args[0].text);
		if (command == "typecheck")
			return Inspector::TypeCheck(args.empty() ? std::string_view() : std::string_view(args[0].text));
		if (command == "instances")
			return args.empty() ? "instances <type> [max]\n" : Inspector::Instances(args[0].text, args.size() > 1 ? static_cast<u32>(ParseInteger(args[1].text).value_or(100)) : 100);
		if (command == "fieldstats")
			return args.empty() ? "fieldstats <type> [max]\n" : Inspector::FieldStats(args[0].text, args.size() > 1 ? static_cast<u32>(ParseInteger(args[1].text).value_or(500)) : 500);
		if (command == "snaps")
			return Inspector::ListSnapshots();
		if (command == "diff")
			return args.empty() ? "diff <name>\n" : Inspector::Diff(args[0].text);

		if (rt.CodeMapEmpty())
			rt.RebuildCodeMap();

		Evaluator ev;
		const auto eval_arg = [&](size_t i) -> std::optional<Value> {
			if (i >= args.size())
			{
				ev.error = "missing argument";
				return std::nullopt;
			}
			return ev.Eval(args[i]);
		};
		const auto int_arg = [&](size_t i, u32 fallback) -> u32 {
			if (i >= args.size())
				return fallback;
			if (const std::optional<s64> n = ParseInteger(args[i].text))
				return static_cast<u32>(*n);
			return fallback;
		};
		// optional trailing "type" argument: an atom that is not a number
		const auto type_arg = [&](size_t i) -> std::string {
			for (; i < args.size(); i++)
			{
				if (args[i].IsList())
					return args[i].Print();
				if (!ParseInteger(args[i].text).has_value())
					return args[i].text;
			}
			return {};
		};
		const auto finish = [&](std::string text) {
			std::string out = ev.output + text;
			if (!ev.error.empty())
				out += "error: " + ev.error + '\n';
			return out;
		};

		if (command == "eval")
		{
			std::string out;
			for (const SExpr& e : args)
			{
				const std::optional<Value> v = ev.Eval(e);
				if (!v.has_value())
					break;
				out += "=> " + FormatValue(*v) + '\n';
			}
			return finish(out);
		}
		if (command == "inspect" || command == "print")
		{
			std::vector<SExpr> form = {Atom(command)};
			form.insert(form.end(), args.begin(), args.end());
			SExpr list;
			list.kind = SExpr::Kind::List;
			list.items = std::move(form);
			const std::optional<Value> v = ev.Eval(list);
			return finish(v.has_value() ? "=> " + FormatValue(*v) + '\n' : std::string());
		}
		if (command == "call")
		{
			SExpr list;
			list.kind = SExpr::Kind::List;
			list.items = args;
			const std::optional<Value> v = ev.Eval(list);
			return finish(v.has_value() ? "=> " + FormatValue(*v) + '\n' : std::string());
		}
		if (command == "method")
		{
			std::vector<SExpr> form = {Atom("call-method")};
			form.insert(form.end(), args.begin(), args.end());
			SExpr list;
			list.kind = SExpr::Kind::List;
			list.items = std::move(form);
			const std::optional<Value> v = ev.Eval(list);
			return finish(v.has_value() ? "=> " + FormatValue(*v) + '\n' : std::string());
		}
		if (command == "dump")
		{
			const std::optional<Value> v = eval_arg(0);
			if (!v.has_value())
				return finish({});
			std::string type = type_arg(1);
			if (type.empty())
				type = ev.TypeOf(*v);
			u32 depth = 1;
			if (args.size() >= 2)
				depth = std::min<u32>(int_arg(args.size() - 1, 1), 8);
			return finish(Inspector::Dump(static_cast<u32>(v->bits), type, depth));
		}
		if (command == "guess")
		{
			const std::optional<Value> v = eval_arg(0);
			if (!v.has_value())
				return finish({});
			std::string type = type_arg(1);
			if (type.empty())
				type = ev.TypeOf(*v);
			return finish(Inspector::Guess(static_cast<u32>(v->bits), int_arg(1, 0), type));
		}
		if (command == "mem")
		{
			const std::optional<Value> v = eval_arg(0);
			return finish(v.has_value() ? Inspector::Hexdump(static_cast<u32>(v->bits), int_arg(1, 0x40)) : std::string());
		}
		if (command == "xref")
		{
			const std::optional<Value> v = eval_arg(0);
			return finish(v.has_value() ? Inspector::Xref(static_cast<u32>(v->bits), std::max<u32>(int_arg(1, 4), 1), int_arg(2, 64)) : std::string());
		}
		if (command == "snap")
		{
			if (args.size() < 2)
				return "snap <name> <expr> [size] [type]\n";
			Evaluator sub;
			const std::optional<Value> v = sub.Eval(args[1]);
			if (!v.has_value())
				return "error: " + sub.error + '\n';
			std::string type = type_arg(2);
			if (type.empty())
				type = sub.TypeOf(*v);
			u32 size = int_arg(2, 0);
			if (size == 0)
			{
				const u32 runtime_type = rt.BasicType(static_cast<u32>(v->bits));
				size = std::max<u32>(db.Size(type), runtime_type ? rt.TypeSize(runtime_type) : 0x40);
			}
			return Inspector::Snapshot(args[0].text, static_cast<u32>(v->bits), size, type);
		}

		return fmt::format("unknown command '{}', try 'help'\n", command);
	}
} // namespace Goal::Repl
