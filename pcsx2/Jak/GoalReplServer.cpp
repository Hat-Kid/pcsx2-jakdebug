// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalReplServer.h"
#include "GoalRepl.h"

#include "Config.h"
#include "Host.h"

#include "common/Console.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace GoalReplServer
{
	namespace
	{
#ifdef _WIN32
		using Socket = SOCKET;
		constexpr Socket BAD_SOCKET = INVALID_SOCKET;
		constexpr int SEND_FLAGS = 0;
		void CloseSocket(Socket s) { closesocket(s); }
		void ShutdownSocket(Socket s) { shutdown(s, SD_BOTH); }

		bool InitializeWinsock()
		{
			static bool initialized = false;
			if (initialized)
				return true;
			WSADATA wsa = {};
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return false;
			initialized = true;
			return true;
		}
#else
		using Socket = int;
		constexpr Socket BAD_SOCKET = -1;
#ifdef MSG_NOSIGNAL
		constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
		constexpr int SEND_FLAGS = 0;
#endif
		void CloseSocket(Socket s) { close(s); }
		void ShutdownSocket(Socket s) { shutdown(s, SHUT_RDWR); }
#endif

		std::thread s_thread;
		std::atomic_bool s_running{false};
		std::atomic<Socket> s_listen{BAD_SOCKET};
		std::atomic<Socket> s_client{BAD_SOCKET};
		u16 s_port = 0;

		bool SendAll(Socket s, const char* data, size_t size)
		{
			while (size > 0)
			{
				const auto sent = send(s, data, static_cast<int>(size), SEND_FLAGS);
				if (sent <= 0)
					return false;
				data += sent;
				size -= static_cast<size_t>(sent);
			}
			return true;
		}

		// the cpu thread may be the one stopping us, so never block on it indefinitely
		std::optional<std::string> RunCommand(std::string line)
		{
			auto promise = std::make_shared<std::promise<std::string>>();
			std::future<std::string> future = promise->get_future();
			Host::RunOnCPUThread([promise, line = std::move(line)]() { promise->set_value(Goal::Repl::Execute(line)); });
			while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
			{
				if (!s_running)
					return std::nullopt;
			}
			return future.get();
		}

		void ServeClient(Socket client)
		{
			std::string buffer;
			char chunk[4096];
			for (;;)
			{
				const auto received = recv(client, chunk, sizeof(chunk), 0);
				if (received <= 0)
					return;
				buffer.append(chunk, static_cast<size_t>(received));

				size_t newline;
				while ((newline = buffer.find('\n')) != std::string::npos)
				{
					std::string line = buffer.substr(0, newline);
					buffer.erase(0, newline + 1);
					if (!line.empty() && line.back() == '\r')
						line.pop_back();

					std::optional<std::string> output = RunCommand(std::move(line));
					if (!output.has_value())
						return;
					output->push_back('\0');
					if (!SendAll(client, output->data(), output->size()))
						return;
				}
			}
		}

		void ThreadMain()
		{
			while (s_running)
			{
				const Socket client = accept(s_listen.load(), nullptr, nullptr);
				if (client == BAD_SOCKET)
				{
					if (!s_running)
						break;
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}
				s_client = client;
				ServeClient(client);
				s_client = BAD_SOCKET;
				CloseSocket(client);
			}
		}
	} // namespace

	bool IsRunning()
	{
		return s_running;
	}

	bool Start(u16 port)
	{
		if (s_running && port == s_port)
			return true;
		Stop();

#ifdef _WIN32
		if (!InitializeWinsock())
		{
			Console.Error("(GoalReplServer) failed to initialize winsock");
			return false;
		}
#endif

		const Socket sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock == BAD_SOCKET)
		{
			Console.Error("(GoalReplServer) failed to create socket");
			return false;
		}

		const int reuse = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(sock, 1) != 0)
		{
			Console.ErrorFmt("(GoalReplServer) could not listen on 127.0.0.1:{}", port);
			CloseSocket(sock);
			return false;
		}

		s_listen = sock;
		s_port = port;
		s_running = true;
		s_thread = std::thread(ThreadMain);
		Console.WriteLnFmt("(GoalReplServer) listening on 127.0.0.1:{}", port);
		return true;
	}

	void Stop()
	{
		if (!s_thread.joinable())
			return;

		s_running = false;
		const Socket listen_sock = s_listen.exchange(BAD_SOCKET);
		if (listen_sock != BAD_SOCKET)
		{
			ShutdownSocket(listen_sock);
			CloseSocket(listen_sock);
		}
		const Socket client = s_client.load();
		if (client != BAD_SOCKET)
			ShutdownSocket(client);
		s_thread.join();
		Console.WriteLn("(GoalReplServer) stopped");
	}

	void ApplySettings()
	{
		if (EmuConfig.EnableJakReplServer)
			Start(static_cast<u16>(Host::GetBaseUIntSettingValue("JakDebug", "ServerPort", DEFAULT_PORT)));
		else
			Stop();
	}
} // namespace GoalReplServer
