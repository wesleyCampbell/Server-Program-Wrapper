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
#include <errno.h>
#include <poll.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <signal.h>

/* -------------- GLOBALS ----------------------- */

#define PROGRAM_PATH "/bin/bash"
#define BUFF_SIZE 1028

#define POLL_TIMEOUT_MS -1
#define SERVER_BACKLOG 1
#define SERVER_PORT 5000

static int master_fd, slave_fd;
static pid_t child_pid;


/* -------------- UTILITY ----------------------- */

void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
volatile sig_atomic_t exit_requested = 0;
volatile sig_atomic_t save_requested = 0;

void handle_sigchld(int sig) {
	child_exited = 1;
}

void handle_sigterm(int sig) {
	exit_requested = 1;
}

void handle_sigint(int sig) {
	exit_requested = 1;
}

void handle_sigusr1(int sig) {
	save_requested = 1;	
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

	while (!child_exited) {
		printf("Listening on port %d...\n", SERVER_PORT);
		// init client connection
		int client_fd = accept(server_fd, NULL, NULL);
		if (client_fd < 0) {
			perror("accept");
			exit(1);
		}
		set_raw(client_fd);
		printf("Client connected.\n");

		// Init buffers
		char buf[BUFF_SIZE];
		struct pollfd poll_fds[2];

		while (1) {

			poll_fds[0].fd = master_fd;
			poll_fds[0].events = POLLIN;

			poll_fds[1].fd = client_fd;
			poll_fds[1].events = POLLIN;

			int ready = poll(poll_fds, 2, POLL_TIMEOUT_MS);

			if (ready == -1) {
				if (errno == EINTR)
					continue;
				perror("poll");
				break;
			}

			// server --> client
			if (poll_fds[0].revents & POLLIN) {
				ssize_t n = read(poll_fds[0].fd, buf, BUFF_SIZE);
				if (n > 0) {
					write(poll_fds[1].fd, buf, n);
				}
				else if (n == 0)
					break;
			}

			// client --> server
			if (poll_fds[1].revents & POLLIN) {
				ssize_t n = read(poll_fds[1].fd, buf, BUFF_SIZE);
				if (n > 0) {
					for (int i = 0; i < n; i++) {
						unsigned char c = buf[i];

						if (c == 0x03) continue;  // Ctl-C
						if (c == 0x1b) continue;  // ESC
												  //
						write(poll_fds[0].fd, &c, 1);
					}	
				}
				else if (n == 0)
					break;  // child exited
			}

			// check for exit
			if (poll_fds[0].revents & (POLLHUP | POLLERR))
				break;
		}

		printf("Client disconnected\n");
		// clean up
		close(client_fd);
	}
	
	printf("Child program no longer active. Shutting server down now...\n");
	close(server_fd);
}

int main() {
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, handle_sigchld);

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
