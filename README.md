# Server Wrapper Program

---

Author: Wesley Campbell
Version: 1.0.1
Date: 2026-04-30

---

A rudimentary wrapper script that wraps an executable binary in a PTY and connects it to a TCP socket.

This allows external connection and control of a CLI program through the TCP protocol. 

Additionally, it is able to recieve UNIX/POSIX signals (such as SIGTERM, SIGINT, SIGUSR1, etc) and have the program
inject custom commands into the CLI program. This allows remote saving and graceful shutdown for programs running inside of containers.

To connect to a program running, one can use `nc` or `screen telnet` or any other TCP communication tool.

Example usage:

`nc <address> <port>`


