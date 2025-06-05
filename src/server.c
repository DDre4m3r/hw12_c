#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/types.h>

#include "server.h"
#if defined(__APPLE__) || defined(__FreeBSD__)
	#include <sys/event.h>
#else
	#include <sys/epoll.h>
	#include <sys/sendfile.h>
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_EVENTS 16
#define READ_BUF   4096

#if defined(__APPLE__) || defined(__FreeBSD__)
	#define USE_KQUEUE 1
#else
	#define USE_EPOLL 1
#endif

#define LOG_ERROR(fmt, ...) fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_PERROR(msg)     perror(msg)
#define LOG_INFO(fmt, ...)  fprintf(stdout, "INFO: " fmt "\n", ##__VA_ARGS__)

struct client {
	int    fd;
	int    file_fd;
	off_t  offset;
	off_t  file_size;
	char   header[256];
	size_t header_len;
	size_t header_sent;
	int    state; /* 0 - reading, 1 - writing */
};

static int make_nonblock(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listener(const char* addr, int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		LOG_PERROR("socket");
		return -1;
	}
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(port);
	if (inet_pton(AF_INET, addr, &sa.sin_addr) <= 0) {
		LOG_PERROR("inet_pton");
		close(fd);
		return -1;
	}
	if (bind(fd, (struct sockaddr*) &sa, sizeof(sa)) < 0) {
		LOG_PERROR("bind");
		close(fd);
		return -1;
	}
	if (listen(fd, SOMAXCONN) < 0) {
		LOG_PERROR("listen");
		close(fd);
		return -1;
	}
	if (make_nonblock(fd) < 0) {
		LOG_PERROR("fcntl");
		close(fd);
		return -1;
	}
	return fd;
}

#if USE_EPOLL
static void close_client(int epfd, struct client* c) {
	epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	if (c->file_fd >= 0) close(c->file_fd);
	free(c);
}
#else
static void close_client(int kq, struct client* c) {
	struct kevent ev[2];
	EV_SET(&ev[0], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	EV_SET(&ev[1], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	kevent(kq, ev, 2, NULL, 0, NULL);
	close(c->fd);
	if (c->file_fd >= 0) close(c->file_fd);
	free(c);
}
#endif

static void send_simple_response(struct client* c, int status, const char* msg) {
	c->file_fd     = -1;
	c->file_size   = 0;
	c->offset      = 0;
	c->header_len  = snprintf(c->header, sizeof(c->header),
	                          "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, msg);
	c->header_sent = 0;
	c->state       = 1;
}

static int build_path(char* dest, size_t size, const char* root, const char* url) {
	while (*url == '/') url++;
	if (strstr(url, "..")) return -1;
	int n = snprintf(dest, size, "%s/%s", root, url);
	if (n < 0 || (size_t) n >= size) return -1;
	return 0;
}

static void prepare_file(struct client* c, const char* root, const char* url) {
	char full[PATH_MAX];
	if (build_path(full, sizeof(full), root, url) < 0) {
		send_simple_response(c, 404, "Not Found");
		return;
	}
	struct stat st;
	if (stat(full, &st) < 0) {
		if (errno == EACCES) send_simple_response(c, 403, "Forbidden");
		else
			send_simple_response(c, 404, "Not Found");
		return;
	}
	if (!S_ISREG(st.st_mode)) {
		send_simple_response(c, 404, "Not Found");
		return;
	}
	if (access(full, R_OK) != 0) {
		send_simple_response(c, 403, "Forbidden");
		return;
	}
	int fd = open(full, O_RDONLY);
	if (fd < 0) {
		if (errno == EACCES) send_simple_response(c, 403, "Forbidden");
		else
			send_simple_response(c, 404, "Not Found");
		return;
	}
	c->file_fd     = fd;
	c->file_size   = st.st_size;
	c->offset      = 0;
	c->header_len  = snprintf(c->header, sizeof(c->header),
	                          "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", (long) st.st_size);
	c->header_sent = 0;
	c->state       = 1;
}

#if USE_EPOLL
static void handle_read(int epfd, struct client* c, const char* root) {
	char    buf[READ_BUF];
	ssize_t n = recv(c->fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0) {
		close_client(epfd, c);
		return;
	}
	buf[n] = '\0';
	char method[16];
	char url[1024];
	if (sscanf(buf, "%15s %1023s", method, url) != 2) {
		close_client(epfd, c);
		return;
	}
	if (strcmp(method, "GET") != 0) {
		send_simple_response(c, 405, "Method Not Allowed");
	} else {
		char* q = strchr(url, '?');
		if (q) *q = '\0';
		prepare_file(c, root, url);
	}
	struct epoll_event ev = {0};
	ev.events             = EPOLLOUT | EPOLLET;
	ev.data.ptr           = c;
	epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

#else
static void handle_read(int kq, struct client* c, const char* root) {
	char    buf[READ_BUF];
	ssize_t n = recv(c->fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0) {
		close_client(kq, c);
		return;
	}
	buf[n] = '\0';
	char method[16];
	char url[1024];
	if (sscanf(buf, "%15s %1023s", method, url) != 2) {
		close_client(kq, c);
		return;
	}
	if (strcmp(method, "GET") != 0) {
		send_simple_response(c, 405, "Method Not Allowed");
	} else {
		char* q = strchr(url, '?');
		if (q) *q = '\0';
		prepare_file(c, root, url);
	}
	struct kevent ev[2];
	EV_SET(&ev[0], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	EV_SET(&ev[1], c->fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, c);
	kevent(kq, ev, 2, NULL, 0, NULL);
}
#endif

#if USE_EPOLL
static void handle_write(int epfd, struct client* c) {
	while (c->header_sent < c->header_len) {
		ssize_t n = send(c->fd, c->header + c->header_sent, c->header_len - c->header_sent, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			close_client(epfd, c);
			return;
		}
		c->header_sent += n;
	}
	if (c->file_fd >= 0) {
		while (c->offset < c->file_size) {
			ssize_t sent = sendfile(c->fd, c->file_fd, &c->offset, c->file_size - c->offset);
			if (sent < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) return;
				close_client(epfd, c);
				return;
			}
		}
		close(c->file_fd);
		c->file_fd = -1;
	}
	close_client(epfd, c);
}

#else
static ssize_t send_file_portable(int sockfd, int filefd, off_t* offset, size_t count) {
	#ifdef __APPLE__
	off_t len = (off_t) count;
	if (sendfile(filefd, sockfd, *offset, &len, NULL, 0) < 0) return -1;
	*offset += len;
	return len;
	#else
	off_t sbytes = 0;
	if (sendfile(filefd, sockfd, *offset, count, NULL, &sbytes, 0) < 0) return -1;
	*offset += sbytes;
	return sbytes;
	#endif
}

static void handle_write(int kq, struct client* c) {
	while (c->header_sent < c->header_len) {
		ssize_t n = send(c->fd, c->header + c->header_sent, c->header_len - c->header_sent, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			close_client(kq, c);
			return;
		}
		c->header_sent += n;
	}
	if (c->file_fd >= 0) {
		while (c->offset < c->file_size) {
			ssize_t sent = send_file_portable(c->fd, c->file_fd, &c->offset, c->file_size - c->offset);
			if (sent < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) return;
				close_client(kq, c);
				return;
			}
		}
		close(c->file_fd);
		c->file_fd = -1;
	}
	close_client(kq, c);
}
#endif

#if USE_EPOLL
static void accept_clients(int epfd, int lfd) {
	while (1) {
		int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
		if (fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			LOG_PERROR("accept");
			break;
		}
		struct client* c = calloc(1, sizeof(*c));
		if (!c) {
			LOG_ERROR("calloc failed");
			close(fd);
			continue;
		}
		c->fd                 = fd;
		c->file_fd            = -1;
		c->state              = 0;
		struct epoll_event ev = {0};
		ev.events             = EPOLLIN | EPOLLET;
		ev.data.ptr           = c;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
			LOG_PERROR("epoll_ctl add");
			close_client(epfd, c);
			continue;
		}
	}
}

#else
static void accept_clients(int kq, int lfd) {
	while (1) {
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			LOG_PERROR("accept");
			break;
		}
		struct client* c = calloc(1, sizeof(*c));
		if (!c) {
			LOG_ERROR("calloc failed");
			close(fd);
			continue;
		}
		c->fd      = fd;
		c->file_fd = -1;
		c->state   = 0;
		struct kevent ev;
		EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, c);
		if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
			LOG_PERROR("kevent add");
			close_client(kq, c);
			continue;
		}
	}
}
#endif

#if USE_EPOLL
int run_server(const struct server_args* args) {
	int lfd = create_listener(args->addr, args->port);
	if (lfd < 0) return -1;
	LOG_INFO("Listening on %s:%d", args->addr, args->port);
	int epfd = epoll_create1(0);
	if (epfd < 0) {
		LOG_PERROR("epoll_create1");
		close(lfd);
		return -1;
	}
	struct epoll_event ev = {0};
	ev.events             = EPOLLIN;
	ev.data.ptr           = NULL; /* listener */
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
		LOG_PERROR("epoll_ctl listener");
		close(lfd);
		close(epfd);
		return -1;
	}
	while (1) {
		struct epoll_event events[MAX_EVENTS];
		int                n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (n < 0) {
			if (errno == EINTR) continue;
			LOG_PERROR("epoll_wait");
			break;
		}
		for (int i = 0; i < n; ++i) {
			if (events[i].data.ptr == NULL) {
				accept_clients(epfd, lfd);
			} else {
				struct client* c = events[i].data.ptr;
				if (events[i].events & EPOLLIN) {
					handle_read(epfd, c, args->path);
				} else if (events[i].events & EPOLLOUT) {
					handle_write(epfd, c);
				} else {
					close_client(epfd, c);
				}
			}
		}
	}
	close(lfd);
	close(epfd);
	return 0;
}

#else
int run_server(const struct server_args* args) {
	int lfd = create_listener(args->addr, args->port);
	if (lfd < 0) return -1;
	LOG_INFO("Listening on %s:%d", args->addr, args->port);
	int kq = kqueue();
	if (kq < 0) {
		LOG_PERROR("kqueue");
		close(lfd);
		return -1;
	}
	struct kevent ev;
	EV_SET(&ev, lfd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
		LOG_PERROR("kevent listener");
		close(lfd);
		close(kq);
		return -1;
	}
	while (1) {
		struct kevent events[MAX_EVENTS];
		int           n = kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);
		if (n < 0) {
			if (errno == EINTR) continue;
			LOG_PERROR("kevent wait");
			break;
		}
		for (int i = 0; i < n; ++i) {
			if (events[i].udata == NULL) {
				accept_clients(kq, lfd);
			} else {
				struct client* c = events[i].udata;
				if (events[i].filter == EVFILT_READ) {
					handle_read(kq, c, args->path);
				} else if (events[i].filter == EVFILT_WRITE) {
					handle_write(kq, c);
				} else {
					close_client(kq, c);
				}
			}
		}
	}
	close(lfd);
	close(kq);
	return 0;
}
#endif