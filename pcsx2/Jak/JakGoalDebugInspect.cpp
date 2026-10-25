// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "JakGoalDebugInspect.h"
#include "GoalRuntime.h"
#include "GoalTypeDb.h"

#include "Config.h"
#include "R5900.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace GoalAnalysis
{
	using namespace Goal;

	std::atomic<u64> g_unique_signatures{0};
	std::atomic<u64> g_calls{0};
	std::atomic<u64> g_functions{0};
	std::atomic<bool> g_active{false};
	bool g_interpreter_hooks = false;

	namespace
	{
		constexpr u32 MAX_SIGNATURES = 64;
		constexpr u32 MAX_RETURNS = 32;
		constexpr u32 MAX_CALLERS = 16;
		constexpr u32 MAX_SHADOW = 4096;
		constexpr u32 ARG_SCAN_INSTRUCTIONS = 128;
		constexpr int REG_A0 = 4;

		struct Obs
		{
			ValueKind kind = ValueKind::Zero;
			u32 detail = 0;
			bool operator==(const Obs&) const = default;
		};

		struct Signature
		{
			std::array<Obs, 8> args{};
			std::array<u64, 8> example{};
			u64 count = 0;
		};

		struct ReturnObs
		{
			Obs obs;
			u64 example = 0;
			u64 count = 0;
		};

		struct FunctionStats
		{
			u64 calls = 0;
			u64 returns = 0;
			u32 arg_count = 0;
			bool signature_overflow = false;
			std::vector<Signature> signatures;
			std::vector<ReturnObs> rets;
			std::vector<u32> callers;
		};

		struct Pending
		{
			u32 fn;
			u32 ret;
			u32 sp;
		};

		bool AllMode()
		{
			return EmuConfig.EnableJakFunctionAnalysis;
		}

		std::unordered_set<u32> s_hook_addrs;
		std::unordered_set<u32> s_trace_entries;
		std::unordered_map<u32, FunctionStats> s_funcs;
		std::vector<Pending> s_shadow;
		u32 s_auto_dump_frames = 600;
		std::mutex s_write_mutex;

		void UpdateInterpreterFlag()
		{
			g_interpreter_hooks = AllMode() || !s_hook_addrs.empty();
			g_active = AllMode() || !s_trace_entries.empty();
		}

		bool EnsureRuntime()
		{
			Runtime& rt = GetRuntime();
			if (rt.IsValid())
				return true;
			// hooks run with the game's own s7 in the register, so no scan needed
			return rt.Refresh(false);
		}

		// keyed loosely so signatures don't explode: which symbol or which pointer doesn't matter
		Obs Observe(u64 value)
		{
			const ValueInfo info = GetRuntime().Classify(value);
			Obs obs{info.kind, 0};
			if (info.kind == ValueKind::Basic)
				obs.detail = info.detail;
			return obs;
		}

		std::string ObsName(const Obs& obs)
		{
			Runtime& rt = GetRuntime();
			switch (obs.kind)
			{
				case ValueKind::Zero:
					return "0";
				case ValueKind::False:
					return "#f";
				case ValueKind::True:
					return "#t";
				case ValueKind::Symbol:
					return "symbol";
				case ValueKind::Basic:
				{
					std::string name = rt.TypeName(obs.detail);
					return name.empty() ? fmt::format("basic@#x{:x}", obs.detail) : name;
				}
				case ValueKind::Type:
					return "type";
				case ValueKind::Function:
					return "function";
				case ValueKind::String:
					return "string";
				case ValueKind::Pair:
					return "pair";
				case ValueKind::EmptyPair:
					return "'()";
				case ValueKind::Pointer:
					return "pointer";
				case ValueKind::ScratchPointer:
					return "scratchpad-pointer";
				case ValueKind::Float:
					return "float";
				case ValueKind::Int:
					return "int";
				case ValueKind::BigInt:
					return "int64";
				case ValueKind::Unknown:
				default:
					return "uint32";
			}
		}

		// arguments a function reads before writing, from a linear scan of its first instructions
		u32 GuessArgCount(u32 fn)
		{
			u32 written = 0;
			s32 max_arg = -1;
			bool stop_after_next = false;
			const auto read = [&](u32 r) {
				if (r >= 4 && r <= 11 && !(written & (1u << r)))
					max_arg = std::max<s32>(max_arg, static_cast<s32>(r) - 4);
			};
			const auto write = [&](u32 r) { written |= (1u << r); };

			for (u32 i = 0; i < ARG_SCAN_INSTRUCTIONS; i++)
			{
				const u32 insn = Mem::Read32(fn + i * 4);
				const u32 op = insn >> 26;
				const u32 rs = (insn >> 21) & 31;
				const u32 rt = (insn >> 16) & 31;
				const u32 rd = (insn >> 11) & 31;
				const u32 funct = insn & 63;

				switch (op)
				{
					case 0x00: // special
						if (funct == 0x08 || funct == 0x09) // jr / jalr
						{
							read(rs);
							if (funct == 0x08 && rs == 31)
								stop_after_next = true;
							break;
						}
						if (funct == 0x00 || funct == 0x02 || funct == 0x03 || funct >= 0x38)
						{
							read(rt);
						}
						else
						{
							read(rs);
							read(rt);
						}
						write(rd);
						break;
					case 0x1c: // mmi
						read(rs);
						read(rt);
						write(rd);
						break;
					case 0x01:
					case 0x06:
					case 0x07:
					case 0x16:
					case 0x17:
						read(rs);
						break;
					case 0x04:
					case 0x05:
					case 0x14:
					case 0x15:
						read(rs);
						read(rt);
						break;
					case 0x0f: // lui
						write(rt);
						break;
					case 0x11: // cop1
					case 0x12: // cop2
						if (rs == 4 || rs == 6 || (op == 0x12 && rs == 5))
							read(rt);
						else if (rs == 0 || rs == 2 || (op == 0x12 && rs == 1))
							write(rt);
						break;
					case 0x1a:
					case 0x1b: // ldl / ldr merge into rt
						read(rs);
						read(rt);
						write(rt);
						break;
					case 0x31:
					case 0x36:
					case 0x39:
					case 0x3e: // fpu / vu loads and stores
						read(rs);
						break;
					default:
						if ((op >= 0x08 && op <= 0x0e) || op == 0x18 || op == 0x19 || (op >= 0x20 && op <= 0x27) || op == 0x1e || op == 0x37)
						{
							read(rs);
							write(rt);
						}
						else if ((op >= 0x28 && op <= 0x2f) || op == 0x1f || op == 0x3f)
						{
							read(rs);
							read(rt);
						}
						break;
				}

				if (stop_after_next && !(op == 0x00 && funct == 0x08))
					break;
			}
			return static_cast<u32>(max_arg + 1);
		}

		std::optional<FunctionSig> DeclaredFunction(const CodeLabel* label)
		{
			if (!label || label->kind != CodeLabel::Kind::Function)
				return std::nullopt;
			const TypeDb& db = GetTypeDb();
			const SExpr* spec = db.FindGlobal(label->name);
			return spec ? db.FunctionSignature(*spec) : std::nullopt;
		}

		const MethodDef* DeclaredMethod(const CodeLabel* label)
		{
			if (!label || label->kind != CodeLabel::Kind::Method)
				return nullptr;
			return GetTypeDb().FindMethod(label->name, label->method_id);
		}

		std::optional<std::vector<std::string>> DeclaredArgs(const CodeLabel* label, std::string* ret)
		{
			if (const std::optional<FunctionSig> sig = DeclaredFunction(label))
			{
				std::vector<std::string> args;
				for (const SExpr& a : sig->args)
					args.push_back(a.Print());
				*ret = sig->ret.Print();
				return args;
			}
			if (const MethodDef* m = DeclaredMethod(label))
			{
				*ret = m->ret;
				return m->args;
			}
			return std::nullopt;
		}

		void OnCall(u32 fn, u32 ret, u32 sp)
		{
			if (!EnsureRuntime())
				return;
			Runtime& rt = GetRuntime();
			g_calls.fetch_add(1, std::memory_order_relaxed);

			FunctionStats& fs = s_funcs[fn];
			if (fs.calls++ == 0)
			{
				g_functions.fetch_add(1, std::memory_order_relaxed);
				if (rt.CodeMapEmpty())
					rt.RebuildCodeMap();
				fs.arg_count = GuessArgCount(fn);
				std::string ret_spec;
				if (const auto declared = DeclaredArgs(rt.LookupCode(fn), &ret_spec))
					fs.arg_count = std::max<u32>(fs.arg_count, static_cast<u32>(declared->size()));
				fs.arg_count = std::min<u32>(fs.arg_count, 8);
			}

			Signature sig;
			for (u32 i = 0; i < fs.arg_count; i++)
			{
				const u64 value = cpuRegs.GPR.r[REG_A0 + i].UD[0];
				sig.args[i] = Observe(value);
				sig.example[i] = value;
			}

			auto it = std::find_if(fs.signatures.begin(), fs.signatures.end(), [&](const Signature& s) { return s.args == sig.args; });
			if (it != fs.signatures.end())
			{
				it->count++;
			}
			else if (fs.signatures.size() < MAX_SIGNATURES)
			{
				sig.count = 1;
				fs.signatures.push_back(sig);
				g_unique_signatures.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				fs.signature_overflow = true;
			}

			if (fs.callers.size() < MAX_CALLERS && std::find(fs.callers.begin(), fs.callers.end(), ret) == fs.callers.end())
				fs.callers.push_back(ret);

			if (s_shadow.size() >= MAX_SHADOW)
				s_shadow.erase(s_shadow.begin(), s_shadow.begin() + MAX_SHADOW / 2);
			s_shadow.push_back({fn, ret, sp});

			// targeted mode: start watching the return site too
			if (s_hook_addrs.insert(ret).second)
			{
				Cpu->Clear(ret, 1);
				UpdateInterpreterFlag();
			}
		}

		void OnReturn(u32 pc)
		{
			const u32 sp = cpuRegs.GPR.n.sp.UL[0];
			const size_t limit = s_shadow.size() > 64 ? s_shadow.size() - 64 : 0;
			for (size_t i = s_shadow.size(); i > limit; i--)
			{
				const Pending& p = s_shadow[i - 1];
				if (p.ret != pc || p.sp != sp)
					continue;

				FunctionStats& fs = s_funcs[p.fn];
				fs.returns++;
				const u64 value = cpuRegs.GPR.n.v0.UD[0];
				const Obs obs = Observe(value);
				auto it = std::find_if(fs.rets.begin(), fs.rets.end(), [&](const ReturnObs& r) { return r.obs == obs; });
				if (it != fs.rets.end())
					it->count++;
				else if (fs.rets.size() < MAX_RETURNS)
					fs.rets.push_back({obs, value, 1});

				// anything above was left by a non-local exit
				s_shadow.resize(i - 1);
				return;
			}
		}

		bool Compatible(std::string_view declared, const Obs& obs, std::string_view method_type)
		{
			if (obs.kind == ValueKind::Zero || declared.empty() || declared == "object" || declared == "none")
				return true;

			const TypeDb& db = GetTypeDb();
			Runtime& rt = GetRuntime();
			std::string decl(declared);
			if (decl == "_type_")
				decl = std::string(method_type);
			if (decl.starts_with("(function") || decl == "function")
				return obs.kind == ValueKind::Function || obs.kind == ValueKind::False;

			const TypeInfo info = db.Info(decl);
			switch (info.category)
			{
				case TypeInfo::Category::Float:
					// only the low 32 bits of a float in a gpr mean anything
					return obs.kind == ValueKind::Float || obs.kind == ValueKind::BigInt;
				case TypeInfo::Category::Integer:
				case TypeInfo::Category::Enum:
				case TypeInfo::Category::Bitfield:
					return obs.kind != ValueKind::Basic && obs.kind != ValueKind::Symbol && obs.kind != ValueKind::String &&
						   obs.kind != ValueKind::Function && obs.kind != ValueKind::Type && obs.kind != ValueKind::Float;
				case TypeInfo::Category::Symbol:
					return obs.kind == ValueKind::Symbol || obs.kind == ValueKind::False || obs.kind == ValueKind::True;
				case TypeInfo::Category::Reference:
					// unknown covers hardware register blocks like dma-bank
					if (obs.kind == ValueKind::False || obs.kind == ValueKind::Pointer || obs.kind == ValueKind::ScratchPointer ||
						obs.kind == ValueKind::Unknown)
						return true;
					if (obs.kind == ValueKind::Basic)
						return rt.TypeInherits(obs.detail, info.name.starts_with("(") ? std::string_view("basic") : std::string_view(info.name)) ||
							   db.Inherits(rt.TypeName(obs.detail), decl);
					if (obs.kind == ValueKind::String)
						return db.Inherits("string", decl);
					if (obs.kind == ValueKind::Type)
						return db.Inherits("type", decl);
					return false;
				default:
					return true;
			}
		}

		std::string FunctionName(u32 fn)
		{
			Runtime& rt = GetRuntime();
			if (const CodeLabel* label = rt.LookupCode(fn))
			{
				if (label->kind == CodeLabel::Kind::Method)
				{
					if (const MethodDef* m = DeclaredMethod(label))
						return fmt::format("(method {} {}) {}", label->method_id, label->name, m->name);
				}
				return rt.CodeLabelName(*label);
			}
			return fmt::format("anon-function@#x{:x}", fn);
		}

		std::string CallerName(u32 ret)
		{
			Runtime& rt = GetRuntime();
			const u32 site = ret - 8;
			if (const auto code = rt.LookupContainingCode(site); code.has_value() && site - code->first < 0x10000)
				return fmt::format("{}+{:#x}", rt.CodeLabelName(*code->second), site - code->first);
			return fmt::format("#x{:x}", site);
		}

		std::vector<std::string> Mismatches(u32 fn, const FunctionStats& fs)
		{
			Runtime& rt = GetRuntime();
			const CodeLabel* label = rt.LookupCode(fn);
			std::string ret_spec;
			const auto declared = DeclaredArgs(label, &ret_spec);
			if (!declared.has_value())
				return {};
			const std::string method_type = (label && label->kind == CodeLabel::Kind::Method) ? label->name : std::string();

			std::vector<std::string> out;
			const bool is_new = label && label->kind == CodeLabel::Kind::Method && label->method_id == 0;
			for (u32 i = 0; i < declared->size() && i < 8; i++)
			{
				// new gets either a heap symbol or the stack address to construct in
				if (is_new && i == 0)
					continue;
				std::set<std::string> bad;
				for (const Signature& s : fs.signatures)
				{
					if (!Compatible((*declared)[i], s.args[i], method_type))
						bad.insert(ObsName(s.args[i]));
				}
				for (const std::string& b : bad)
					out.push_back(fmt::format("arg{} declared {} but saw {}", i, (*declared)[i], b));
			}
			if (ret_spec != "none")
			{
				for (const ReturnObs& r : fs.rets)
				{
					if (!Compatible(ret_spec, r.obs, method_type))
						out.push_back(fmt::format("return declared {} but saw {}", ret_spec, ObsName(r.obs)));
				}
			}
			if (fs.arg_count < declared->size())
				out.push_back(fmt::format("declared {} args, the code only seems to read {}", declared->size(), fs.arg_count));
			return out;
		}

		std::optional<u32> ResolveTarget(std::string_view target, std::vector<u32>* many)
		{
			Runtime& rt = GetRuntime();
			if (const std::optional<s64> addr = ParseInteger(target))
				return static_cast<u32>(*addr);

			// type:method-name or type:id
			if (const size_t colon = target.find(':'); colon != std::string_view::npos)
			{
				const std::string_view type_name = target.substr(0, colon);
				const std::string_view method = target.substr(colon + 1);
				const std::optional<u32> type = rt.FindType(type_name);
				if (!type.has_value())
					return std::nullopt;
				s32 id = -1;
				if (const std::optional<s64> n = ParseInteger(method))
					id = static_cast<s32>(*n);
				else if (const MethodDef* m = GetTypeDb().FindMethodByName(type_name, method))
					id = m->id;
				if (id < 0 || static_cast<u32>(id) >= rt.TypeMethodCount(*type))
					return std::nullopt;
				return rt.TypeMethod(*type, id);
			}

			const std::optional<u32> sym = rt.FindSymbol(target);
			if (!sym.has_value())
				return std::nullopt;
			const u32 value = rt.SymbolValue(*sym);
			if (rt.IsFunction(value))
				return value;

			// a type: every method it implements itself
			if (rt.IsType(value) && many)
			{
				const u32 parent = rt.TypeParent(value);
				for (u32 i = 0; i < rt.TypeMethodCount(value); i++)
				{
					const u32 m = rt.TypeMethod(value, i);
					const bool inherited = i < rt.TypeMethodCount(parent) && rt.TypeMethod(parent, i) == m;
					if (rt.IsFunction(m) && !inherited)
						many->push_back(m);
				}
				return many->empty() ? std::nullopt : std::optional<u32>(many->front());
			}
			return std::nullopt;
		}
	} // namespace

	bool ShouldHookCallSites()
	{
		return AllMode();
	}

	bool HasHookAddresses()
	{
		return !s_hook_addrs.empty();
	}

	bool IsHookAddress(u32 pc)
	{
		return s_hook_addrs.contains(pc);
	}

	void NoteCallSite(u32 return_addr)
	{
		if (s_hook_addrs.insert(return_addr).second)
			UpdateInterpreterFlag();
	}

	void OnCallSite(u32 return_addr)
	{
		if (g_eeIsolatedExecution)
			return;
		OnCall(cpuRegs.GPR.n.t9.UL[0], return_addr, cpuRegs.GPR.n.sp.UL[0]);
	}

	void OnHookAddress(u32 pc)
	{
		if (g_eeIsolatedExecution)
			return;
		if (!s_shadow.empty())
			OnReturn(pc);
		if (!AllMode() && s_trace_entries.contains(pc))
			OnCall(pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0]);
	}

	void InterpreterCheck(u32 pc)
	{
		if (s_hook_addrs.contains(pc))
			OnHookAddress(pc);
	}

	std::string Trace(const std::vector<std::string>& targets)
	{
		Runtime& rt = GetRuntime();
		if (!rt.Refresh())
			return rt.Error() + '\n';
		if (rt.CodeMapEmpty())
			rt.RebuildCodeMap();

		std::string out;
		for (const std::string& target : targets)
		{
			std::vector<u32> many;
			const std::optional<u32> addr = ResolveTarget(target, &many);
			if (!addr.has_value())
			{
				out += fmt::format("can't resolve '{}' (use a function name, type, type:method or #xaddr)\n", target);
				continue;
			}
			if (many.empty())
				many.push_back(*addr);
			for (const u32 fn : many)
			{
				s_trace_entries.insert(fn);
				s_hook_addrs.insert(fn);
				Cpu->Clear(fn, 1);
				out += fmt::format("tracing {} @ #x{:x}\n", FunctionName(fn), fn);
			}
		}
		UpdateInterpreterFlag();
		return out;
	}

	std::string Untrace(const std::vector<std::string>& targets)
	{
		if (targets.empty() || (targets.size() == 1 && targets[0] == "all"))
		{
			const size_t n = s_trace_entries.size();
			s_trace_entries.clear();
			s_hook_addrs.clear();
			s_shadow.clear();
			Cpu->Reset();
			UpdateInterpreterFlag();
			return fmt::format("stopped tracing {} function(s)\n", n);
		}

		std::string out;
		for (const std::string& target : targets)
		{
			std::vector<u32> many;
			const std::optional<u32> addr = ResolveTarget(target, &many);
			if (!addr.has_value())
				continue;
			if (many.empty())
				many.push_back(*addr);
			for (const u32 fn : many)
			{
				if (s_trace_entries.erase(fn))
				{
					s_hook_addrs.erase(fn);
					Cpu->Clear(fn, 1);
					out += fmt::format("stopped tracing {}\n", FunctionName(fn));
				}
			}
		}
		UpdateInterpreterFlag();
		return out;
	}

	std::string ListTraced()
	{
		std::string out = fmt::format("all call sites: {}\n", AllMode() ? "on" : "off");
		for (const u32 fn : s_trace_entries)
			out += fmt::format("  {} @ #x{:x}\n", FunctionName(fn), fn);
		return out;
	}

	void OnSettingsChanged(bool all_mode_enabled)
	{
		if (!all_mode_enabled)
		{
			// drop return sites, keep the targeted entries
			s_hook_addrs = s_trace_entries;
			s_shadow.clear();
		}
		UpdateInterpreterFlag();
	}

	void Reset()
	{
		s_funcs.clear();
		s_shadow.clear();
		g_functions = 0;
		g_unique_signatures = 0;
		g_calls = 0;
	}

	void SetAutoDump(u32 frames)
	{
		s_auto_dump_frames = frames;
	}

	void OnVSync(u32 frame)
	{
		UpdateInterpreterFlag();
		if (!AllMode() && s_trace_entries.empty())
			return;

		if (frame % 60 == 0)
			GetRuntime().Refresh(false);

		if (s_auto_dump_frames != 0 && frame % s_auto_dump_frames == 0 && !s_funcs.empty())
			WriteJson({}, true);
	}

	std::string Show(std::string_view target)
	{
		Runtime& rt = GetRuntime();
		if (!rt.Refresh())
			return rt.Error() + '\n';
		rt.RebuildCodeMap();

		std::vector<u32> many;
		const std::optional<u32> addr = ResolveTarget(target, &many);
		if (!addr.has_value())
			return fmt::format("can't resolve '{}'\n", target);
		if (many.empty())
			many.push_back(*addr);

		std::string out;
		for (const u32 fn : many)
		{
			const auto it = s_funcs.find(fn);
			out += fmt::format("{} @ #x{:x}\n", FunctionName(fn), fn);

			std::string ret_spec;
			if (const auto declared = DeclaredArgs(rt.LookupCode(fn), &ret_spec))
			{
				std::string args;
				for (const std::string& a : *declared)
					args += (args.empty() ? "" : " ") + a;
				out += fmt::format("  all-types: ({}) -> {}\n", args, ret_spec);
			}

			if (it == s_funcs.end())
			{
				out += "  no calls recorded\n";
				continue;
			}
			const FunctionStats& fs = it->second;
			out += fmt::format("  {} call(s), reads {} arg register(s){}\n", fs.calls, fs.arg_count,
				fs.signature_overflow ? ", too many signatures to keep them all" : "");

			std::vector<const Signature*> sigs;
			for (const Signature& s : fs.signatures)
				sigs.push_back(&s);
			std::sort(sigs.begin(), sigs.end(), [](const Signature* a, const Signature* b) { return a->count > b->count; });
			for (const Signature* s : sigs)
			{
				std::string types, examples;
				for (u32 i = 0; i < fs.arg_count; i++)
				{
					types += (i ? " " : "") + ObsName(s->args[i]);
					examples += (i ? ", " : "") + rt.Describe(s->example[i]);
				}
				out += fmt::format("  {:>8}x ({})\n             e.g. {}\n", s->count, types, examples);
			}

			if (!fs.rets.empty())
			{
				out += fmt::format("  returns ({} seen):", fs.returns);
				for (const ReturnObs& r : fs.rets)
					out += fmt::format(" {} x{} [{}]", ObsName(r.obs), r.count, rt.Describe(r.example));
				out += '\n';
			}

			std::string callers;
			for (const u32 c : fs.callers)
				callers += (callers.empty() ? "" : ", ") + CallerName(c);
			out += fmt::format("  called from: {}\n", callers);

			for (const std::string& m : Mismatches(fn, fs))
				out += "  !! " + m + '\n';
		}
		return out;
	}

	std::string Top(u32 count)
	{
		Runtime& rt = GetRuntime();
		if (rt.Refresh())
			rt.RebuildCodeMap();
		std::vector<std::pair<u64, u32>> order;
		for (const auto& [fn, fs] : s_funcs)
			order.emplace_back(fs.calls, fn);
		std::sort(order.rbegin(), order.rend());
		std::string out;
		for (size_t i = 0; i < order.size() && i < count; i++)
		{
			const FunctionStats& fs = s_funcs[order[i].second];
			out += fmt::format("  {:>10} {:<50} {} sig(s)\n", order[i].first, FunctionName(order[i].second), fs.signatures.size());
		}
		return out.empty() ? "nothing recorded yet\n" : out;
	}

	std::string Stats()
	{
		return fmt::format("all call sites: {}, traced entries: {}, hook addresses: {}\n{} functions seen, {} unique signatures, {} calls\n",
			AllMode() ? "on" : "off", s_trace_entries.size(), s_hook_addrs.size(), s_funcs.size(), g_unique_signatures.load(),
			g_calls.load());
	}

	std::string OverlayText()
	{
		if (!g_active.load(std::memory_order_relaxed))
			return {};
		return fmt::format("Jak Analysis - Functions: {} | Signatures: {} | Calls: {}", g_functions.load(std::memory_order_relaxed),
			g_unique_signatures.load(std::memory_order_relaxed), g_calls.load(std::memory_order_relaxed));
	}

	std::string WriteJson(std::string path, bool background)
	{
		Runtime& rt = GetRuntime();
		if (!rt.Refresh(false))
			return rt.Error() + '\n';
		rt.RebuildCodeMap();

		if (path.empty())
			path = Path::Combine(EmuFolders::DataRoot, "jak_call_analysis.json");

		rapidjson::Document doc;
		doc.SetObject();
		auto& alloc = doc.GetAllocator();
		const auto str = [&](const std::string& s) { return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), alloc); };

		doc.AddMember("game", str(GameVersionName(rt.Version())), alloc);
		rapidjson::Value functions(rapidjson::kArrayType);
		for (const auto& [fn, fs] : s_funcs)
		{
			rapidjson::Value f(rapidjson::kObjectType);
			f.AddMember("name", str(FunctionName(fn)), alloc);
			f.AddMember("address", str(fmt::format("#x{:x}", fn)), alloc);
			f.AddMember("calls", fs.calls, alloc);
			f.AddMember("arg_count", fs.arg_count, alloc);

			std::string ret_spec;
			if (const auto declared = DeclaredArgs(rt.LookupCode(fn), &ret_spec))
			{
				rapidjson::Value d(rapidjson::kArrayType);
				for (const std::string& a : *declared)
					d.PushBack(str(a), alloc);
				f.AddMember("declared_args", d, alloc);
				f.AddMember("declared_return", str(ret_spec), alloc);
			}

			rapidjson::Value sigs(rapidjson::kArrayType);
			for (const Signature& s : fs.signatures)
			{
				rapidjson::Value sig(rapidjson::kObjectType);
				sig.AddMember("count", s.count, alloc);
				rapidjson::Value args(rapidjson::kArrayType);
				rapidjson::Value examples(rapidjson::kArrayType);
				for (u32 i = 0; i < fs.arg_count; i++)
				{
					args.PushBack(str(ObsName(s.args[i])), alloc);
					examples.PushBack(str(rt.Describe(s.example[i])), alloc);
				}
				sig.AddMember("args", args, alloc);
				sig.AddMember("examples", examples, alloc);
				sigs.PushBack(sig, alloc);
			}
			f.AddMember("signatures", sigs, alloc);

			rapidjson::Value rets(rapidjson::kArrayType);
			for (const ReturnObs& r : fs.rets)
			{
				rapidjson::Value ro(rapidjson::kObjectType);
				ro.AddMember("type", str(ObsName(r.obs)), alloc);
				ro.AddMember("count", r.count, alloc);
				ro.AddMember("example", str(rt.Describe(r.example)), alloc);
				rets.PushBack(ro, alloc);
			}
			f.AddMember("returns", rets, alloc);

			rapidjson::Value callers(rapidjson::kArrayType);
			for (const u32 c : fs.callers)
				callers.PushBack(str(CallerName(c)), alloc);
			f.AddMember("callers", callers, alloc);

			rapidjson::Value mismatches(rapidjson::kArrayType);
			for (const std::string& m : Mismatches(fn, fs))
				mismatches.PushBack(str(m), alloc);
			f.AddMember("mismatches", mismatches, alloc);

			functions.PushBack(f, alloc);
		}
		doc.AddMember("functions", functions, alloc);

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		doc.Accept(writer);
		std::string json(buffer.GetString(), buffer.GetSize());

		const auto write = [](std::string path, std::string json) {
			std::lock_guard lock(s_write_mutex);
			FileSystem::WriteStringToFile(path.c_str(), json);
		};
		if (background)
		{
			std::thread(write, path, std::move(json)).detach();
			return {};
		}
		write(path, std::move(json));
		return fmt::format("wrote {} function(s) to {}\n", s_funcs.size(), path);
	}
} // namespace GoalAnalysis
