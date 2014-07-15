#include "ccextractor.h"
#include "networking.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>

#define DEBUG_OUT 0

/* Protocol constants: */
#define INT_LEN         10
#define OK              1
#define PASSWORD        2
#define CAPTIONS        3
#define PROGRAM         4
#define ERROR           51
#define UNKNOWN_COMMAND 52
#define WRONG_PASSWORD  53
#define CONN_LIMIT      54

int srv_sd = -1; /* Server socket descriptor */

/*
 * Established connection to speciefied addres.
 * Returns socked id
 */
int tcp_connect(const char *addr, const char *port);

/*
 * Asks password from stdin, sends it to the server and waits for
 * it's response
 */
int ask_passwd(int sd);

#define BUF_SIZE 20480
char *buf;
char *buf_end;
void init_buf();

/*
 * Writes data according to protocol to descriptor
 * block format:
 * command | lenght        | data         | \r\n
 * 1 byte  | INT_LEN bytes | lenght bytes | 2 bytes
 */
ssize_t write_block(int fd, char command, const char *buf, size_t buf_len);

/* Reads n bytes from descriptor */
ssize_t readn(int fd, void *vptr, size_t n);

/* Writes n bytes to descriptor */
ssize_t writen(int fd, const void *vptr, size_t n);

/* Convinence functions */
ssize_t write_byte(int fd, char status);
ssize_t read_byte(int fd, char *status);

#if DEBUG_OUT
void pr_command(char c);
#endif

void connect_to_srv(const char *addr, const char *port)
{
	if (NULL == addr)
	{
		mprint("Server addres is not set\n");
		fatal(EXIT_FAILURE, "Unable to connect\n");
	}

	if (NULL == port)
		port = "2048";

	mprint("\n");
	mprint("Connecting to %s:%s\n", addr, port);

	if ((srv_sd = tcp_connect(addr, port)) < 0)
		fatal(EXIT_FAILURE, "Unable to connect\n");

	if (ask_passwd(srv_sd) < 0)
		fatal(EXIT_FAILURE, "Unable to connect\n");

	mprint("Connected to %s:%s\n", addr, port);
}

void init_buf()
{
	buf = (char *) malloc(BUF_SIZE);
	if (NULL == buf)
		fatal(EXIT_NOT_ENOUGH_MEMORY, "malloc error(): %s", strerror(errno));

	buf_end = buf;
}

void net_append_cc(const char *fmt, ...)
{
	if (NULL == buf)
		init_buf();

	va_list args;
	va_start(args, fmt);

	int rc = vsnprintf(buf_end, BUF_SIZE - (buf_end - buf), fmt, args);
	if (rc < 0)
	{
		mprint("net_append_cc() error: can\'t append ");
		mprint(fmt, args);
		mprint("\n");
		return;
	}

	buf_end += rc;

	va_end(args);
}

void net_append_cc_n(const char *data, size_t len)
{
	assert(data != NULL);
	assert(len > 0);

	if (NULL == buf)
		init_buf();

	size_t nleft = BUF_SIZE - (buf_end - buf);
	if (nleft < len) 
	{
		mprint("net_append_cc_n() warning: buffer overflow, pruning %zd bytes\n",
				len - nleft);
		len = nleft;
	}

	memcpy(buf_end, data, len);

	buf_end += len;
}

void net_send_cc()
{
	assert(srv_sd > 0);

	if (buf_end - buf == 0)
		return;

	if (write_block(srv_sd, CAPTIONS, buf, buf_end - buf) < 0)
	{
		mprint("Can't send subtitle block\n");
		return; // XXX: store somewhere
	}

	buf_end = buf;

	char ok;
	if (read_byte(srv_sd, &ok) != 1)
		return;

#if DEBUG_OUT
	fprintf(stderr, "[S] ");
	pr_command(ok);
	fprintf(stderr, "\n");
#endif

	if (ERROR == ok)
	{
		mprint("Internal server error\n"); 
		return;
	}

	sleep(1);

	return;
}

void net_set_new_program(const char *name)
{
	assert(name != NULL);
	assert(srv_sd > 0);

	size_t len = strlen(name) - 1; /* without '\0' */

	if (write_block(srv_sd, PROGRAM, name, len) < 0)
	{
		mprint("Can't send new program name to the server\n");
		return; // XXX: store somewhere
	}
}

/*
 * command | lenght        | data         | \r\n
 * 1 byte  | INT_LEN bytes | lenght bytes | 2 bytes
 */
ssize_t write_block(int fd, char command, const char *buf, size_t buf_len)
{
	assert(buf != NULL);
	assert(buf_len > 0);

#if DEBUG_OUT
	fprintf(stderr, "[C] ");
#endif

	int rc;
	ssize_t nwritten = 0;

	if ((rc = write_byte(fd, command)) < 0)
		return -1;
	else if (rc != 1)
		return 0;
	nwritten++;

#if DEBUG_OUT
	pr_command(command);
	fprintf(stderr, " ");
#endif

	char len_str[INT_LEN] = {0};
	snprintf(len_str, INT_LEN, "%zd", buf_len);
	if ((rc = writen(fd, len_str, INT_LEN)) < 0)
		return -1;
	else if (rc != INT_LEN)
		return 0;
	nwritten += rc;

#if DEBUG_OUT
	fwrite(len_str, sizeof(char), INT_LEN, stderr);
	fprintf(stderr, " ");
#endif

	if ((rc = writen(fd, buf, buf_len)) < 0)
		return -1;
	else if (rc != (int) buf_len)
		return 0;
	nwritten += rc;

#if DEBUG_OUT
	fwrite(buf, sizeof(char), buf_len - 2, stderr);
	fprintf(stderr, " ");
#endif

	if ((rc = write_byte(fd, '\r')) < 0)
		return -1;
	else if (rc != 1)
		return 0;
	nwritten++;

#if DEBUG_OUT
	fprintf(stderr, "\\r");
#endif

	if ((rc = write_byte(fd, '\n')) < 0)
		return -1;
	else if (rc != 1)
		return 0;
	nwritten++;

#if DEBUG_OUT
	fprintf(stderr, "\\n\n");
#endif

	return nwritten;
}

int tcp_connect(const char *host, const char *port)
{
	assert(host != NULL);
	assert(port != NULL);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *ai;
	int rc = getaddrinfo(host, port, &hints, &ai);
	if (rc != 0) {
		mprint("getaddrinfo() error: %s\n", gai_strerror(rc));
		return -1;
	}

	struct addrinfo *p;
	int sockfd;
	/* Try each address until we sucessfully connect */
	for (p = ai; p != NULL; p = p->ai_next) {
		sockfd = socket(p->ai_family, SOCK_STREAM, p->ai_protocol);

		if (-1 == sockfd) {
			mprint("socket() error: %s\n", strerror(errno));
			if (p->ai_next != NULL)
				mprint("trying next addres ...");

			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0)
			break;

		mprint("connect() error: %s\n", strerror(errno));
		if (p->ai_next != NULL)
			mprint("trying next addres ...");

		close(sockfd);
	}

	freeaddrinfo(ai);

	if (NULL == p)
		return -1;

	return sockfd;
}

int ask_passwd(int sd)
{
	assert(srv_sd > 0);

	struct termios old, new;
	int rc;
	size_t len = 0;
	char *pw = NULL;

	char ok;

	do {
		do {
			if (read_byte(sd, &ok) != 1)
			{
				fatal(EXIT_FAILURE, "read() error: %s", strerror(errno));
			}

#if DEBUG_OUT
			fprintf(stderr, "[S] ");
			pr_command(ok);
			fprintf(stderr, "\n");
#endif

			if (OK == ok)
			{
				return 1;
			}
			else if (CONN_LIMIT == ok)
			{
				mprint("Too many connections to the server, try later\n");
				return -1;
			} 
			else if (ERROR == ok)
			{
				mprint("Internal server error\n");
				return -1;
			}

		} while(ok != PASSWORD);

		printf("Enter password: ");
		fflush(stdout);

		if (tcgetattr(STDIN_FILENO, &old) != 0)
		{
			mprint("tcgetattr() error: %s\n", strerror(errno));
		}

		new = old;
		new.c_lflag &= ~ECHO;
		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new) != 0)
		{
			mprint("tcgetattr() error: %s\n", strerror(errno));
		}

		rc = getline(&pw, &len, stdin);
		rc--; /* -1 for \n */

		if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &old) != 0)
		{
			mprint("tcgetattr() error: %s\n", strerror(errno));
		}

		printf("\n");
		fflush(stdout);

		if (write_block(sd, PASSWORD, pw, rc) < 0)
			return -1;

		if (read_byte(sd, &ok) != 1)
			return -1;

#if DEBUG_OUT
		fprintf(stderr, "[S] ");
		pr_command(ok);
		fprintf(stderr, "\n");
#endif

		if (UNKNOWN_COMMAND == ok)
		{
			printf("Wrong password\n");
			fflush(stdout);
		}
		else if (ERROR == ok)
		{
			mprint("Internal server error\n");
			return -1;
		}
	} while(OK != ok);

	return 1;
}

#if DEBUG_OUT
void pr_command(char c)
{
	switch(c)
	{
		case CAPTIONS:
			fprintf(stderr, "CAPTIONS");
			break;
		case OK:
			fprintf(stderr, "OK");
			break;
		case WRONG_PASSWORD:
			fprintf(stderr, "WRONG_PASSWORD");
			break;
		case UNKNOWN_COMMAND:
			fprintf(stderr, "UNKNOWN_COMMAND");
			break;
		case ERROR:
			fprintf(stderr, "ERROR");
			break;
		case PROGRAM:
			fprintf(stderr, "PROGRAM");
			break;
		case CONN_LIMIT:
			fprintf(stderr, "CONN_LIMIT");
			break;
		case PASSWORD:
			fprintf(stderr, "PASSWORD");
			break;
		default:
			fprintf(stderr, "UNKNOWN (%d)", (int) c);
			break;
	}
}
#endif

ssize_t readn(int fd, void *vptr, size_t n)
{
	size_t nleft;
	ssize_t nread;
	char *ptr;

	ptr = vptr;
	nleft = n;
	while (nleft > 0)
	{
		if (NULL == vptr) {
			char c;
			nread = read(fd, &c, 1);
		}
		else
		{
			nread = read(fd, ptr, nleft);
		}

		if (nread < 0)
		{
			if (errno == EINTR)
			{
				nread = 0;
			}
			else
			{
				mprint("read() error: %s\n", strerror(errno));
				return -1;
			}
		}
		else if (0 == nread)
		{
			break; /* EOF */
		}

		nleft -= nread;
		ptr += nread;
	}

	return n - nleft;
}

ssize_t writen(int fd, const void *vptr, size_t n)
{
	assert(vptr != NULL);
	assert(n > 0);

	size_t nleft;
	ssize_t nwritten;
	const char *ptr;

	ptr = vptr;
	nleft = n;
	while (nleft > 0)
	{
		if ((nwritten = write(fd, ptr, nleft)) < 0) 
		{
			if (errno == EINTR)
			{
				nwritten = 0;
			}
			else
			{
				mprint("write() error: %s\n", strerror(errno));
				return -1;
			}
		}
		else if (0 == nwritten)
		{
			break;
		}

		nleft -= nwritten;
		ptr += nwritten;
	}

	return n;
}

ssize_t write_byte(int fd, char ch)
{
	return writen(fd, &ch, 1);
}

ssize_t read_byte(int fd, char *ch)
{
	assert(ch != 0);

	return readn(fd, ch, 1);
}
