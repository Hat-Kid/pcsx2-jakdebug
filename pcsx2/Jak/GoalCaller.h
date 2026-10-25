// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Calls GOAL functions inside the running game. The call is executed synchronously by
// the interpreter with events and interrupts held off, then every register, the scratch
// region and the scratchpad are restored, so the game continues as if nothing happened
// (apart from whatever memory the called function changed itself).
//
// (format #t ...) is redirected into a scratch string while the call runs, which is how
// the output of inspect/print methods is captured.
namespace Goal
{
	struct CallOptions
	{
		u64 max_instructions = 50'000'000;
		std::optional<u32> pp; // process pointer (s6) to run with
	};

	struct CallResult
	{
		bool ok = false;
		u64 ret = 0;
		u64 instructions = 0;
		std::string output;
		std::string error;
		std::vector<std::string> notes;
	};

	class Caller
	{
	public:
		static constexpr u32 SCRATCH_SIZE = 0x20000;

		CallResult Call(u32 function, std::span<const u64> args, const CallOptions& options = {});

		// queues a GOAL string that will exist during the next Call(), returns its address
		std::optional<u32> AllocTempString(std::string_view str);

		// 0 means automatic: top of 128mb ram in dev mode, below the stack pointer otherwise
		void SetScratchBase(u32 base) { m_scratch_override = base; }
		u32 ScratchOverride() const { return m_scratch_override; }
		std::optional<u32> ResolveScratchBase(std::string* error) const;

		void ResetCalibration();
		std::string Status() const;

	private:
		struct Frame
		{
			u32 ret;
			u32 sp;
			bool nested;
		};

		void WriteString(u32 obj, u32 capacity, std::string_view value);
		bool Run(u32 function, std::span<const u64> args, u32 base, bool redirect, const CallOptions& options, CallResult& result);
		void Calibrate(u32 base, const CallOptions& options);

		u32 m_scratch_override = 0;
		std::vector<std::string> m_temp_strings;

		u32 m_calibrated_s7 = 0;
		bool m_calibration_tried = false;
		u32 m_print_pending_addr = 0;
	};

	Caller& GetCaller();
} // namespace Goal
