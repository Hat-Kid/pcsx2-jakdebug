// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalSymbolNames.h"
#include "Lzo.h"

#include "CDVD/IsoReader.h"
#include "Config.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <cstring>
#include <span>

namespace Goal::SymbolNames
{
	namespace
	{
		constexpr u32 DGO_HEADER_SIZE = 0x40;
		constexpr u32 LZO_CHUNK_SIZE = 0x8000;
		constexpr u32 LINK_V5 = 5;
		constexpr u32 LINK_HEADER_V5_SIZE = 0x50;
		constexpr std::string_view EMPTY_PAIR_NAME = "_empty_";

		struct State
		{
			std::string key; // serial and crc of the disc the names belong to
			Map names;
			std::string source;
			std::string error;
			bool tried_cache = false;
			bool tried_disc = false;
		};

		State s_state;

		u32 Read32(std::span<const u8> data, size_t offset)
		{
			u32 value;
			std::memcpy(&value, &data[offset], sizeof(value));
			return value;
		}

		// "oZlB" archives (jak x) are a sequence of lzo chunks, chunks that didn't compress are stored as is
		bool Decompress(std::span<const u8> in, std::vector<u8>* out)
		{
			if (in.size() < 8)
				return false;
			const u32 total = Read32(in, 4);
			out->resize(total);
			size_t in_pos = 8;
			size_t out_pos = 0;
			while (out_pos < total)
			{
				u32 chunk = 0;
				while (chunk == 0)
				{
					if (in_pos + 4 > in.size())
						return false;
					chunk = Read32(in, in_pos);
					in_pos += 4;
				}

				if (chunk < LZO_CHUNK_SIZE)
				{
					if (in_pos + chunk > in.size())
						return false;
					size_t written = 0;
					if (lzokay::decompress(&in[in_pos], chunk, out->data() + out_pos, total - out_pos, written) != lzokay::EResult::Success)
						return false;
					in_pos += chunk;
					out_pos += written;
				}
				else
				{
					const size_t size = std::min<size_t>(LZO_CHUNK_SIZE, total - out_pos);
					if (in_pos + size > in.size())
						return false;
					std::memcpy(out->data() + out_pos, &in[in_pos], size);
					in_pos += LZO_CHUNK_SIZE;
					out_pos += size;
				}
				in_pos = (in_pos + 3) & ~static_cast<size_t>(3);
			}
			return true;
		}

		// v5 link data, see jak-project decompiler/ObjectFile/LinkedObjectFileCreation.cpp link_v5().
		// reads past the end return 0, which terminates every table
		void ParseObject(std::span<const u8> obj, Map* names)
		{
			const auto byte = [&](size_t i) -> u8 { return i < obj.size() ? obj[i] : 0; };
			if (obj.size() < LINK_HEADER_V5_SIZE || (byte(8) | (byte(9) << 8)) != LINK_V5)
				return;

			const u32 link = Read32(obj, 0xc);
			const u32 segments = std::min<u32>(byte(0x14), 3);
			for (u32 seg = 0; seg < segments; seg++)
			{
				const size_t info = static_cast<size_t>(link) + 16 * seg;
				if (info + 16 > obj.size())
					return;
				if (Read32(obj, info + 8) == 0)
					continue;

				// pointer links: alternating seek/fix counts, 0xff continues a count
				size_t p = static_cast<size_t>(link) + Read32(obj, info);
				if (byte(p))
				{
					while (true)
					{
						while (byte(p) == 0xff)
						{
							p++;
							if (byte(p) == 0)
								p++;
						}
						p++;
						if (byte(p) == 0)
							break;
					}
				}
				p++;

				// symbol links: kind, u16 id (offset from s7 + 1), name, variable length reference offsets
				while (byte(p))
				{
					const s32 id = static_cast<s16>(byte(p + 1) | (byte(p + 2) << 8));
					size_t end = p + 3;
					while (byte(end))
						end++;
					if (end >= obj.size())
						return;
					const std::string_view name(reinterpret_cast<const char*>(&obj[p + 3]), end - p - 3);
					if (name != EMPTY_PAIR_NAME)
						names->emplace(id - 1, name);

					p = end + 1;
					do
					{
						const u8 v = byte(p);
						p += (v & 3) == 0 ? 1 : (v & 2) == 0 ? 2 : (v & 1) == 0 ? 3 : 4;
					} while (byte(p));
					p++;
				}
			}
		}

		bool ParseArchive(std::span<const u8> data, Map* names)
		{
			std::vector<u8> decompressed;
			if (data.size() >= 4 && std::memcmp(data.data(), "oZlB", 4) == 0)
			{
				if (!Decompress(data, &decompressed))
					return false;
				data = decompressed;
			}
			if (data.size() < DGO_HEADER_SIZE)
				return false;

			const u32 count = Read32(data, 0);
			size_t p = DGO_HEADER_SIZE;
			for (u32 i = 0; i < count && p + DGO_HEADER_SIZE <= data.size(); i++)
			{
				const size_t size = std::min<size_t>(Read32(data, p), data.size() - p - DGO_HEADER_SIZE);
				p += DGO_HEADER_SIZE;
				ParseObject(data.subspan(p, size), names);
				p += (size + 15) & ~static_cast<size_t>(15);
			}
			return true;
		}

		bool IsArchiveName(std::string_view name)
		{
			return StringUtil::EndsWithNoCase(name, ".DGO") || StringUtil::EndsWithNoCase(name, ".CGO");
		}

		bool ReadDisc(Map* names, u32* archives, std::string* error)
		{
			IsoReader iso;
			Error err;
			if (!iso.Open(&err))
			{
				*error = "can't read the disc: " + err.GetDescription();
				return false;
			}

			std::vector<u8> data;
			for (const char* dir : {"DGO", "CGO"})
			{
				for (const std::string& path : iso.GetFilesInDirectory(dir))
				{
					const std::string_view name = Path::GetFileName(path);
					if (!IsArchiveName(name) || !iso.ReadFile(fmt::format("{}/{}", dir, name), &data))
						continue;
					*archives += ParseArchive(data, names) ? 1 : 0;
				}
			}
			return true;
		}

		bool ReadDirectory(const std::string& directory, Map* names, u32* archives, std::string* error)
		{
			FileSystem::FindResultsArray files;
			if (!FileSystem::FindFiles(directory.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &files))
			{
				*error = fmt::format("no files in {}", directory);
				return false;
			}
			for (const FILESYSTEM_FIND_DATA& file : files)
			{
				if (!IsArchiveName(file.FileName))
					continue;
				if (const std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(file.FileName.c_str()))
					*archives += ParseArchive(*data, names) ? 1 : 0;
			}
			return true;
		}

		std::string CurrentKey()
		{
			const std::string serial = VMManager::GetDiscSerial();
			return serial.empty() ? std::string() : fmt::format("{}_{:08X}", serial, VMManager::GetDiscCRC());
		}

		std::string CachePath(const std::string& key)
		{
			return Path::Combine(EmuFolders::Cache, fmt::format("jak_symbols_{}.txt", key));
		}

		bool LoadCache(const std::string& key)
		{
			const std::optional<std::string> text = FileSystem::ReadFileToString(CachePath(key).c_str());
			if (!text.has_value())
				return false;
			for (const std::string_view line : StringUtil::SplitString(*text, '\n'))
			{
				const size_t space = line.find(' ');
				if (space == std::string_view::npos)
					continue;
				if (const std::optional<s32> offset = StringUtil::FromChars<s32>(line.substr(0, space)))
					s_state.names.emplace(*offset, line.substr(space + 1));
			}
			s_state.source = CachePath(key);
			return !s_state.names.empty();
		}

		void SaveCache(const std::string& key)
		{
			std::string text;
			for (const auto& [offset, name] : s_state.names)
				text += fmt::format("{} {}\n", offset, name);
			if (!FileSystem::WriteStringToFile(CachePath(key).c_str(), text))
				Console.WarningFmt("Jak: couldn't write the symbol name cache to {}", CachePath(key));
		}

		bool Load(const std::string& key, const std::string& directory)
		{
			Common::Timer timer;
			Map names;
			u32 archives = 0;
			std::string error;
			const bool ok = directory.empty() ? ReadDisc(&names, &archives, &error) : ReadDirectory(directory, &names, &archives, &error);
			if (ok && names.empty())
				error = fmt::format("no symbols found in {} DGO/CGO files", archives);
			if (!error.empty())
			{
				s_state.error = error;
				Console.ErrorFmt("Jak: symbol names: {}", error);
				return false;
			}

			s_state.names = std::move(names);
			s_state.error.clear();
			s_state.source = directory.empty() ? "disc" : directory;
			Console.WriteLnFmt("Jak: recovered {} symbol names from {} DGO/CGO files in {:.1f}s", s_state.names.size(), archives, timer.GetTimeSeconds());
			SaveCache(key);
			return true;
		}

		void ResetIfDiscChanged(const std::string& key)
		{
			if (key == s_state.key)
				return;
			s_state = {};
			s_state.key = key;
		}
	} // namespace

	const Map* ForDisc(bool allow_disc_read)
	{
		const std::string key = CurrentKey();
		if (key.empty())
			return nullptr;
		ResetIfDiscChanged(key);

		if (s_state.names.empty() && !s_state.tried_cache)
		{
			s_state.tried_cache = true;
			LoadCache(key);
		}
		if (s_state.names.empty() && allow_disc_read && !s_state.tried_disc)
		{
			s_state.tried_disc = true;
			Console.WriteLn("Jak: this build doesn't keep symbol names in memory, reading them from the disc...");
			Load(key, {});
		}
		return s_state.names.empty() ? nullptr : &s_state.names;
	}

	std::string Reload(std::string_view directory)
	{
		const std::string key = CurrentKey();
		if (key.empty())
			return "no disc\n";
		ResetIfDiscChanged(key);
		s_state.tried_cache = true;
		s_state.tried_disc = true;
		if (!Load(key, std::string(directory)))
			return fmt::format("failed: {}\n", s_state.error);
		return Status();
	}

	std::string Status()
	{
		if (!s_state.names.empty())
			return fmt::format("{} symbol names recovered from {}\n", s_state.names.size(), s_state.source);
		if (!s_state.error.empty())
			return fmt::format("symbol names could not be recovered: {}\n", s_state.error);
		return "symbol names: not needed yet (all names are in memory) or not loaded\n";
	}
} // namespace Goal::SymbolNames
