#include <asm-generic/ioctls.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <pty.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <poll.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <signal.h>
#include <sys/eventfd.h>

/* -------------- GLOBALS ----------------------- */

#define PROGRAM_PATH "/bin/bash"
#define BUFF_SIZE 1028

#define POLL_TIMEOUT_MS -1
#define SERVER_BACKLOG 1
#define SERVER_PORT 5000

#define SAVE_COMMAND "\nsave\n"
#define EXIT_COMMAND "\nexit\n"

enum SIGNAL_FLAGS {
	SAVE=0x01,
	EXIT=0x02,
};

static int master_fd, slave_fd;
static pid_t child_pid;


/* -------------- UTILITY ----------------------- */

void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

ssize_t writeAll(int fd, char* buff, int buff_len) {
	int written = 0;
	while (written < buff_len) {
		ssize_t n = write(fd, buff + written, buff_len - written);
		
		if (n > 0) {
			written += n;
		} else if (n == -1) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
		}

		return -1;
	}

	return written;
}

void injectCommand(char* cmd) {
	printf("Injecting command `%s` to the pty...", cmd);
	writeAll(master_fd, cmd, strlen(cmd));
}

/**
 * Sets up a socket on a given port and returns its file descriptor
 *
 * @param port The port to run the socket on
 * @return The fd of the socket
 */
static int setup_server(int port) {
	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) {
		perror("socket");
		exit(1);
	}

	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// Initialize the socket
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	// bind the socket to the address and port
	if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(1);
	}

	if (listen(server_fd, SERVER_BACKLOG) < 0) {
		perror("listen");
		exit(1);
	}

	return server_fd;
}

/**
 * Sets a given file descriptor as raw, meaning it transmits the raw bytes
 * over the wire
 *
 * @param fd The file descriptor to make raw
 */
static void set_raw(int fd) {
	struct termios tty;
	if (tcgetattr(fd, &tty) == 0) {
		cfmakeraw(&tty);
		tty.c_lflag &= ~ECHO;
		tcsetattr(fd, TCSANOW, &tty);
	}
}

/* -------------- INTERUPTS ----------------------- */

volatile sig_atomic_t child_exited = 0;

int event_fd;

void handle_sigchld(int sig) {
	child_exited = 1;
}

void handle_sigterm(int sig) {
	uint64_t val = EXIT;
	write(event_fd, &val, sizeof(val));
}

void handle_sigint(int sig) {
	uint64_t val = EXIT;
	write(event_fd, &val, sizeof(val));
}

void handle_sigusr1(int sig) {
	uint64_t val = SAVE;
	write(event_fd, &val, sizeof(val));
}

void handle_sigusr2(int sig) {

}

/* -------------- MAIN ----------------------- */

/**
 * As the child, run the CLI program in a PTY
 */
void manage_pty() {
	close(master_fd);

	// Make slave the controlling terminal
	setsid();
	/*ioctl(STDIN_FILENO, TIOCSCTTY, 0);*/

	// Map STDIN to the slave
	if (dup2(slave_fd, STDIN_FILENO) < 0) 
		goto dup2_fail;
	if (dup2(slave_fd, STDOUT_FILENO) < 0) 
		goto dup2_fail;
	if (dup2(slave_fd, STDERR_FILENO) < 0)
		goto dup2_fail;

	set_raw(STDIN_FILENO);

	// close the slave now that it was mapped out
	close(slave_fd);

	// runs the program
	execlp(PROGRAM_PATH, PROGRAM_PATH, NULL);
	perror("execlp");
	exit(1);

dup2_fail:
	perror("dup2");
	exit(1);
}	

void manage_bridge() {
	close(slave_fd);

	// Init server 
	int server_fd = setup_server(SERVER_PORT);
	event_fd = eventfd(0, EFD_NONBLOCK);
	int client_fd = -1;
	printf("Starting server on port %d\n", SERVER_PORT);


	// set file descriptors as non-blocking
	set_nonblocking(master_fd);
	set_nonblocking(server_fd);


	bool client_connected = 0;

	// Set up the individual poll structures
	struct pollfd poll_fds[4]; 
	// reset the memory
	memset(poll_fds, 0, sizeof(poll_fds));

	// Initialize the poll structs
	poll_fds[0] = (struct pollfd) { .fd = master_fd, .events = POLLIN };
	poll_fds[1] = (struct pollfd) { .fd = server_fd, .events = POLLIN };
	poll_fds[3] = (struct pollfd) { .fd = event_fd, .events = POLLIN };

	while (!child_exited) {
		// Initialize the client connection fd if there is a client connected else leave it null (essentially)
		poll_fds[2].fd = client_connected ? client_fd : -1;
		poll_fds[2].events = client_connected ? POLLIN : 0;

		// Wait until one of the structs has been updated
		int ready = poll(poll_fds, 4, POLL_TIMEOUT_MS);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("poll");
			break;
		}

		// Client is trying to make a connection
		if (poll_fds[1].revents & POLLIN) {
			int fd = accept(server_fd, NULL, NULL);
			if (fd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) break;
				perror("accept");
				break;
			}

			if (!client_connected) {
				client_connected = true;
				client_fd = fd;
				set_nonblocking(client_fd);
				printf("Client connected...");
			} else {
				close(fd);  // reject any additional clients that try to connect
			}
		}

		// There is an event that needs handling
		if (poll_fds[3].revents & POLLIN) {
			uint64_t val;
			printf("Signal recieved...\n");
			while (read(event_fd, &val, sizeof(val)) == sizeof(val)) {
				if (val == EXIT) {
					printf("BEGINNING EXIT PROCESS: \n");

					injectCommand(SAVE_COMMAND);
					injectCommand(EXIT_COMMAND);
				}
				if (val == SAVE) {
					printf("BEGINNING SAVE PROCESS: \n");

					injectCommand(SAVE_COMMAND);
				}
			}
		}

		// Server has updates to send to client
		if (poll_fds[0].revents & POLLIN && client_connected) {
			char buff[BUFF_SIZE];
			ssize_t n = read(master_fd, buff, BUFF_SIZE);	
			if (n > 0) {
				writeAll(client_fd, buff, n);
			} else if (n == 0) {
				client_connected = false;
				close(client_fd);
				continue;
			} else {  // n < 0
				if (errno == EAGAIN || errno == EINTR) continue;
				break;
			}
		}

		// Client has updates to send to server
		if (poll_fds[2].revents & POLLIN) {
			char buff[BUFF_SIZE];
			ssize_t n = read(client_fd, buff, BUFF_SIZE);
			if (n > 0) {
				writeAll(master_fd, buff, n);
			} else if (n == 0) {
				client_connected = false;
				close(client_fd);
				continue;
			} else {  // n < 0
				if (errno == EINTR || errno == EAGAIN) continue;
				break;
			}
		}

		// Check for termination
		for (int i = 0; i < 4; i++) {
			if (poll_fds[i].revents & (POLLHUP | POLLERR)) {
				if (i == 0) {  // master 
					child_exited = 1;
				}
				else if (i == 2) { // client
					close(client_fd);
					client_connected = false;
				}
				printf("fd %d has failed\n", i);
				break;
			}
		}
	}



	// cleanup
	if (client_connected) 
		close(client_fd);
	if (!child_exited) {
		injectCommand(SAVE_COMMAND);
		injectCommand(EXIT_COMMAND);
	}
	close(event_fd);
	close(server_fd);
}

int main() {
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, handle_sigchld);
	signal(SIGTERM, handle_sigterm);
	signal(SIGUSR1, handle_sigusr1);

	// Open PTY
	int pty_ret = openpty(&master_fd, &slave_fd, NULL, NULL, NULL);
	if (pty_ret == -1) {
		perror("openpty");
		exit(1);
	}	

	// Fork CLI process
	int pid = fork();
	if (pid < 0) {
		perror("fork failed");
		exit(1);
	}

	/* --- CHILD --- */
	if (pid == 0) {
		manage_pty();
	} else {
		manage_bridge();
	}

	/* ---- CLEANUP ---- */
	close(master_fd);
	wait(NULL);
	return 0;
}
