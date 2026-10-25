#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Client for the PCSX2 GOAL REPL server (Tools > Jak Debugging > Enable GOAL REPL Server).

Interactive:   goal_repl.py
One command:   goal_repl.py "(inspect *target*)"
Script:        goal_repl.py -f commands.txt
As a library:  from goal_repl import GoalRepl; GoalRepl().run("dump *target*")
"""

import argparse
import os
import socket
import sys


class GoalRepl:
    def __init__(self, host="127.0.0.1", port=28012, timeout=60.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buffer = b""

    def run(self, command: str) -> str:
        self.sock.sendall(command.replace("\n", " ").encode() + b"\n")
        while b"\0" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("PCSX2 closed the connection")
            self.buffer += chunk
        reply, _, self.buffer = self.buffer.partition(b"\0")
        return reply.decode(errors="replace")

    def close(self):
        self.sock.close()


def interactive(repl: GoalRepl):
    try:
        import readline

        history = os.path.expanduser("~/.goal_repl_history")
        try:
            readline.read_history_file(history)
        except OSError:
            pass
    except ImportError:
        readline = None

    try:
        while True:
            try:
                line = input("goal> ")
            except EOFError:
                break
            if line.strip() in ("quit", "exit"):
                break
            if line.strip():
                sys.stdout.write(repl.run(line))
    finally:
        if readline:
            readline.write_history_file(history)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="*", help="command to run, interactive if omitted")
    parser.add_argument("-f", "--file", help="run every line of this file")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=28012)
    args = parser.parse_args()

    repl = GoalRepl(args.host, args.port)
    try:
        if args.file:
            with open(args.file) as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith(";"):
                        print(f"> {line}")
                        sys.stdout.write(repl.run(line))
        elif args.command:
            sys.stdout.write(repl.run(" ".join(args.command)))
        else:
            interactive(repl)
    finally:
        repl.close()


if __name__ == "__main__":
    main()
