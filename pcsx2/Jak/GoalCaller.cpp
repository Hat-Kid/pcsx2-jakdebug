// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalCaller.h"
#include "GoalRuntime.h"

#include "R5900.h"
#include "VU.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <random>

namespace Goal
{
	// scratch region layout. strings are basics, so the object is 4 bytes after a 16 byte aligned tag.
	static constexpr u32 STUB_OFFSET = 0x0;
	static constexpr u32 OUTPUT_OBJ = 0x14;
	static constexpr u32 OUTPUT_CAPACITY = 0x8000;
	static constexpr u32 NESTED_OBJ = 0x8024;
	static constexpr u32 NESTED_CAPACITY = 0x2000;
	static constexpr u32 TEMP_START = 0xa040;
	static constexpr u32 TEMP_END = 0xc000;
	static constexpr u32 STACK_TOP = Caller::SCRATCH_SIZE - 0x40;

	static constexpr u32 SCRATCHPAD_BASE = 0x70000000;
	static constexpr u32 SCRATCHPAD_SIZE = 0x4000;
	static constexpr u32 DEV_RAM_SCRATCH = 0x07f00000;
	// between the bios kernel (ends around 0x84000) and the game elf at ELF_BASE, unused by the jak games.
	// needs proper testing with other games/bios versions
	static constexpr u32 LOW_RAM_SCRATCH = 0x000c0000;
	static constexpr u32 ELF_BASE = 0x00100000;

	static constexpr int REG_A0 = 4;
	static constexpr int REG_S6 = 22;

	Caller& GetCaller()
	{
		static Caller s_caller;
		return s_caller;
	}

	namespace
	{
		struct SavedVU0
		{
			VECTOR VF[32];
			REG_VI VI[32];
			VECTOR ACC;
			REG_VI q;
			REG_VI p;

			void Save()
			{
				std::memcpy(VF, VU0.VF, sizeof(VF));
				std::memcpy(VI, VU0.VI, sizeof(VI));
				ACC = VU0.ACC;
				q = VU0.q;
				p = VU0.p;
			}

			void Restore() const
			{
				std::memcpy(VU0.VF, VF, sizeof(VF));
				std::memcpy(VU0.VI, VI, sizeof(VI));
				VU0.ACC = ACC;
				VU0.q = q;
				VU0.p = p;
			}
		};

		u32 AlignUp(u32 value, u32 alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		u32 TempStringSize(std::string_view str)
		{
			// tag + allocated-length + chars + terminator
			return AlignUp(8 + static_cast<u32>(str.size()) + 1, 16);
		}

		bool Overlaps(u32 a_start, u32 a_size, u32 b_start, u32 b_end)
		{
			return a_start < b_end && b_start < a_start + a_size;
		}
	} // namespace

	std::optional<u32> Caller::ResolveScratchBase(std::string* error) const
	{
		u32 base;
		if (m_scratch_override != 0)
		{
			base = m_scratch_override;
		}
		else if (Mem::RamSize() > DEV_RAM_SCRATCH + SCRATCH_SIZE)
		{
			// the jak debug mode patches put the stack at the top of 128mb and the debug heap ends
			// 1mb below it, so the bottom of that stack area is free
			base = DEV_RAM_SCRATCH;
		}
		else
		{
			// retail: the global heap covers nearly all of ram, including the main stack
			base = LOW_RAM_SCRATCH;
		}

		if ((base & 0xf) != 0 || !Mem::Valid(base, SCRATCH_SIZE) || !Mem::IsRam(base))
		{
			*error = fmt::format("scratch area #x{:x} is not 16 byte aligned main ram", base);
			return std::nullopt;
		}

		Runtime& rt = GetRuntime();
		for (const char* heap : {"global", "debug"})
		{
			const std::optional<u32> sym = rt.FindSymbol(heap);
			if (!sym.has_value())
				continue;
			const u32 kheap = rt.SymbolValue(*sym);
			if (!Mem::Valid(kheap, 16))
				continue;
			const u32 heap_base = Mem::Read32(kheap);
			const u32 heap_end = Mem::Read32(kheap + 12);
			if (heap_base < heap_end && Overlaps(base, SCRATCH_SIZE, heap_base, heap_end))
			{
				*error = fmt::format("scratch area #x{:x} overlaps the {} heap [#x{:x}, #x{:x}), choose another with 'scratch <addr>'",
					base, heap, heap_base, heap_end);
				return std::nullopt;
			}
		}

		return base;
	}

	std::optional<u32> Caller::AllocTempString(std::string_view str)
	{
		std::string error;
		const std::optional<u32> base = ResolveScratchBase(&error);
		if (!base.has_value())
			return std::nullopt;

		u32 offset = TEMP_START;
		for (const std::string& s : m_temp_strings)
			offset += TempStringSize(s);
		if (offset + TempStringSize(str) > TEMP_END)
			return std::nullopt;

		m_temp_strings.emplace_back(str);
		return *base + offset + 4;
	}

	void Caller::ResetCalibration()
	{
		m_calibration_tried = false;
		m_calibrated_s7 = 0;
		m_print_pending_addr = 0;
	}

	std::string Caller::Status() const
	{
		std::string error;
		const std::optional<u32> base = ResolveScratchBase(&error);
		std::string out = base.has_value() ? fmt::format("scratch area: #x{:x}-#x{:x}{}\n", *base, *base + SCRATCH_SIZE,
												 m_scratch_override ? "" : " (automatic)") :
											 fmt::format("scratch area: unavailable ({})\n", error);
		if (!m_calibration_tried)
			out += "print buffer: not calibrated yet (happens on the first call)\n";
		else if (m_print_pending_addr)
			out += fmt::format("print buffer: PrintPending lives at #x{:x}\n", m_print_pending_addr);
		else
			out += "print buffer: not found, nested print output may be out of order\n";
		return out;
	}

	void Caller::WriteString(u32 obj, u32 capacity, std::string_view value)
	{
		const u32 tag = GetRuntime().StringType();
		Mem::Write32(obj - 4, tag);
		Mem::Write32(obj, capacity);
		Mem::Write(obj + 4, value.data(), static_cast<u32>(value.size()));
		const u8 terminator = 0;
		Mem::Write(obj + 4 + static_cast<u32>(value.size()), &terminator, 1);
	}

	bool Caller::Run(u32 function, std::span<const u64> args, u32 base, bool redirect, const CallOptions& options, CallResult& result)
	{
		Runtime& rt = GetRuntime();

		const std::array<u8, 16> stub = {};
		Mem::Write(base + STUB_OFFSET, stub.data(), static_cast<u32>(stub.size()));
		WriteString(base + OUTPUT_OBJ, OUTPUT_CAPACITY, {});
		WriteString(base + NESTED_OBJ, NESTED_CAPACITY, {});
		u32 temp_offset = TEMP_START;
		for (const std::string& s : m_temp_strings)
		{
			WriteString(base + temp_offset + 4, static_cast<u32>(s.size()) + 1, s);
			temp_offset += TempStringSize(s);
		}

		for (u32 i = 0; i < 8; i++)
		{
			cpuRegs.GPR.r[REG_A0 + i].UD[0] = i < args.size() ? args[i] : 0;
			cpuRegs.GPR.r[REG_A0 + i].UD[1] = 0;
		}
		cpuRegs.GPR.n.s7.UD[0] = rt.S7();
		cpuRegs.GPR.n.t9.UD[0] = function;
		cpuRegs.GPR.n.ra.UD[0] = base + STUB_OFFSET;
		cpuRegs.GPR.n.sp.UD[0] = base + STACK_TOP;

		if (options.pp.has_value())
		{
			cpuRegs.GPR.r[REG_S6].UD[0] = *options.pp;
		}
		else
		{
			const u32 pp = cpuRegs.GPR.r[REG_S6].UL[0];
			if (!rt.TypeInherits(rt.BasicType(pp), "process"))
				cpuRegs.GPR.r[REG_S6].UD[0] = rt.S7();
		}

		cpuRegs.pc = function;
		cpuRegs.branch = 0;
		cpuRegs.IsDelaySlot = 0;

		u32 format_fns[2] = {};
		if (redirect)
		{
			for (int i = 0; const char* name : {"format", "_format"})
			{
				if (const std::optional<u32> sym = rt.FindSymbol(name))
				{
					const u32 fn = rt.SymbolValue(*sym);
					if (rt.IsFunction(fn))
						format_fns[i] = fn;
				}
				i++;
			}
		}

		const u32 output_obj = base + OUTPUT_OBJ;
		const u32 nested_obj = base + NESTED_OBJ;
		std::vector<Frame> frames;
		u64 steps = 0;
		bool ok = false;

		g_eeIsolatedFault = false;
		g_eeIsolatedSyscall = -1;
		g_eeIsolatedTty.clear();
		g_eeIsolatedExecution = true;
		for (;;)
		{
			const u32 pc = cpuRegs.pc;
			if (pc == base + STUB_OFFSET)
			{
				ok = true;
				break;
			}
			if (g_eeIsolatedFault)
			{
				if (g_eeIsolatedSyscall >= 0)
					result.error = fmt::format("the call made syscall {} ({}) near pc #x{:x}, which can't run without the bios kernel",
						g_eeIsolatedSyscall, R5900::bios[g_eeIsolatedSyscall], pc);
				else
					result.error = fmt::format("memory fault near pc #x{:x} {}", pc, rt.DescribeAddress(pc));
				break;
			}
			if (pc >= 0x80000000u)
			{
				result.error = fmt::format("the call raised an exception (pc #x{:x}, epc #x{:x})", pc, cpuRegs.CP0.n.EPC);
				break;
			}
			if (steps >= options.max_instructions)
			{
				result.error = fmt::format("gave up after {} instructions (pc #x{:x} {})", steps, pc, rt.DescribeAddress(pc));
				break;
			}

			if (redirect)
			{
				const u32 sp = cpuRegs.GPR.n.sp.UL[0];
				if (!frames.empty() && pc == frames.back().ret && sp == frames.back().sp)
				{
					const bool nested = frames.back().nested;
					frames.pop_back();
					if (nested)
					{
						// a print method ran (format #t ...) while an outer format was half done. put its
						// text back into the print buffer where the outer format expects to find it.
						const std::string text = Mem::ReadCString(nested_obj + 4, NESTED_CAPACITY);
						const u32 pending = m_print_pending_addr ? Mem::Read32(m_print_pending_addr) : 0;
						if (!text.empty() && pending && Mem::Valid(pending, static_cast<u32>(text.size()) + 1) &&
							Mem::ReadCString(pending + 1, static_cast<u32>(text.size())) == std::string_view(text).substr(1))
						{
							Mem::Write(pending, text.c_str(), static_cast<u32>(text.size()) + 1);
						}
						else if (!text.empty())
						{
							const std::string current = Mem::ReadCString(output_obj + 4, OUTPUT_CAPACITY);
							WriteString(output_obj, OUTPUT_CAPACITY, current + text);
							if (result.notes.empty())
								result.notes.push_back("nested print output could not be put back in place and may appear out of order");
						}
					}
				}

				if ((pc == format_fns[0] || pc == format_fns[1]) && pc != 0 && cpuRegs.GPR.r[REG_A0].UL[0] == rt.TrueSym())
				{
					const bool nested = !frames.empty();
					if (nested)
						WriteString(nested_obj, NESTED_CAPACITY, {});
					cpuRegs.GPR.r[REG_A0].UD[0] = nested ? nested_obj : output_obj;
					frames.push_back({cpuRegs.GPR.n.ra.UL[0], sp, nested});
				}
			}

			intStepIsolated();
			steps++;
		}
		g_eeIsolatedExecution = false;
		g_eeIsolatedFault = false;

		if (!g_eeIsolatedTty.empty())
		{
			result.notes.push_back("kernel tty output: " + g_eeIsolatedTty);
			g_eeIsolatedTty.clear();
		}

		result.instructions += steps;
		result.ok = ok;
		result.ret = cpuRegs.GPR.n.v0.UD[0];
		return ok;
	}

	void Caller::Calibrate(u32 base, const CallOptions& options)
	{
		Runtime& rt = GetRuntime();
		const std::optional<u32> format_sym = rt.FindSymbol("format");
		if (!format_sym.has_value())
			return;
		const u32 format_fn = rt.SymbolValue(*format_sym);
		if (!rt.IsFunction(format_fn))
			return;

		std::random_device rd;
		const std::string marker = fmt::format("@@jakdbg-{:08x}{:08x}@@", rd(), rd());

		std::vector<std::string> saved_temp = std::move(m_temp_strings);
		m_temp_strings = {marker};
		const u64 args[2] = {base + OUTPUT_OBJ, base + TEMP_START + 4};
		CallResult result;
		const bool ok = Run(format_fn, args, base, false, options, result);
		m_temp_strings = std::move(saved_temp);
		if (!ok)
			return;

		// format leaves "\0" + marker[1:] at the old end of the print buffer
		const std::span<const u8> ram = Mem::Ram();
		const std::vector<u8> tail(marker.begin() + 1, marker.end());
		const std::boyer_moore_horspool_searcher searcher(tail.begin(), tail.end());
		u32 pending = 0;
		for (auto it = ram.begin();;)
		{
			it = std::search(it, ram.end(), searcher);
			if (it == ram.end())
				break;
			const u32 addr = static_cast<u32>(it - ram.begin());
			if (addr > 0 && ram[addr - 1] == 0 && !Overlaps(addr, 1, base, base + SCRATCH_SIZE))
			{
				pending = addr - 1;
				break;
			}
			++it;
		}
		if (!pending)
			return;

		// the global is in the kernel's .data/.bss, between the elf base and the global heap.
		// the bios keeps copies of registers below the elf, which can hold the same value
		u32 search_end = static_cast<u32>(ram.size());
		if (const std::optional<u32> global = rt.FindSymbol("global"))
		{
			const u32 heap_base = Mem::Read32(rt.SymbolValue(*global));
			if (heap_base > ELF_BASE && heap_base < search_end)
				search_end = heap_base;
		}
		for (u32 addr = ELF_BASE; addr + 4 <= search_end; addr += 4)
		{
			u32 word;
			std::memcpy(&word, &ram[addr], 4);
			if (word == pending && !Overlaps(addr, 4, base, base + SCRATCH_SIZE))
			{
				m_print_pending_addr = addr;
				break;
			}
		}
	}

	CallResult Caller::Call(u32 function, std::span<const u64> args, const CallOptions& options)
	{
		CallResult result;
		Runtime& rt = GetRuntime();
		if (!rt.Refresh())
		{
			result.error = rt.Error();
			m_temp_strings.clear();
			return result;
		}
		if (args.size() > 8)
		{
			result.error = "GOAL functions take at most 8 register arguments";
			m_temp_strings.clear();
			return result;
		}

		std::string error;
		const std::optional<u32> base = ResolveScratchBase(&error);
		if (!base.has_value())
		{
			result.error = error;
			m_temp_strings.clear();
			return result;
		}

		const cpuRegisters saved_cpu = cpuRegs;
		const fpuRegisters saved_fpu = fpuRegs;
		SavedVU0 saved_vu0;
		saved_vu0.Save();
		std::vector<u8> saved_region(SCRATCH_SIZE);
		Mem::Read(*base, saved_region.data(), SCRATCH_SIZE);
		std::vector<u8> saved_scratchpad(SCRATCHPAD_SIZE);
		Mem::Read(SCRATCHPAD_BASE, saved_scratchpad.data(), SCRATCHPAD_SIZE);

		if (m_calibrated_s7 != rt.S7())
		{
			m_calibrated_s7 = rt.S7();
			m_calibration_tried = false;
			m_print_pending_addr = 0;
		}
		if (!m_calibration_tried)
		{
			m_calibration_tried = true;
			Calibrate(*base, options);
			cpuRegs = saved_cpu;
			fpuRegs = saved_fpu;
		}

		// text printed by kernel code without going through format (cprintf) lands at the end
		// of the print buffer
		u32 pending_end = 0;
		if (m_print_pending_addr)
		{
			const u32 pending = Mem::Read32(m_print_pending_addr);
			if (pending && Mem::Valid(pending))
				pending_end = pending + static_cast<u32>(Mem::ReadCString(pending, 0x10000).size());
		}

		Run(function, args, *base, true, options, result);

		result.output = Mem::ReadCString(*base + OUTPUT_OBJ + 4, OUTPUT_CAPACITY);
		if (pending_end)
		{
			const std::string direct = Mem::ReadCString(pending_end, 0x10000);
			if (!direct.empty())
			{
				result.output += direct;
				const u8 terminator = 0;
				Mem::Write(pending_end, &terminator, 1);
			}
		}

		Mem::Write(*base, saved_region.data(), SCRATCH_SIZE);
		Mem::Write(SCRATCHPAD_BASE, saved_scratchpad.data(), SCRATCHPAD_SIZE);
		cpuRegs = saved_cpu;
		fpuRegs = saved_fpu;
		saved_vu0.Restore();
		m_temp_strings.clear();
		return result;
	}
} // namespace Goal
