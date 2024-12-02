/*
 * tunnel.c - Setup a local port forwarding through remote shadowsocks server
 *
 * Copyright (C) 2013 - 2019, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef __MINGW32__
#include <arpa/inet.h>
#include <errno.h>
#include <linux/tcp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <assert.h>
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(HAVE_SYS_IOCTL_H) && defined(HAVE_NET_IF_H) && defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#define SET_INTERFACE
#endif

/* Return a pointer to a @c struct, given a pointer to one of its
 * fields. */
#define cork_container_of(field, struct_type, field_name) ((struct_type *) (-offsetof(struct_type, field_name) + (void *) (field)))

#ifdef CONNECT_IN_PROGRESS
#undef CONNECT_IN_PROGRESS
#endif
#define CONNECT_IN_PROGRESS EAGAIN // EAGAIN: linux only

#define MAX_CONNECT_TIMEOUT 10
#define MAX_REMOTE_NUM 10

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

// #include "netutils.h"
#include "utils.h"
// #include "plugin.h"
#include "tunnel.h"
#include <string.h>
#ifdef WIN32
#include "winsock.h"
#endif

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

static void accept_cb(EV_P_ ev_io *w, int revents);
static void server_recv_cb(EV_P_ ev_io *w, int revents);
static void server_send_cb(EV_P_ ev_io *w, int revents);
static void remote_recv_cb(EV_P_ ev_io *w, int revents);
static void remote_send_cb(EV_P_ ev_io *w, int revents);

static remote_t *new_remote(int fd, int timeout);
static server_t *new_server(int fd);

static void free_remote(remote_t *remote);
static void close_and_free_remote(EV_P_ remote_t *remote);
static void free_server(server_t *server);
static void close_and_free_server(EV_P_ server_t *server);

#ifdef __ANDROID__
int vpn = 0;
#endif

int verbose             = 0;
int reuse_port          = 0;
int tcp_incoming_sndbuf = 0;
int tcp_incoming_rcvbuf = 0;
int tcp_outgoing_sndbuf = 0;
int tcp_outgoing_rcvbuf = 0;

#ifdef XXXX // 2024年12月02日 13:22:08
static crypto_t *crypto;
#endif

static int ipv6first = 0;
static int mode      = TCP_ONLY;
#ifdef HAVE_SETRLIMIT
static int nofile = 0;
#endif
static int no_delay  = 0;
int        fast_open = 0;
static int ret_val   = 0;

static struct ev_signal sigint_watcher;
static struct ev_signal sigterm_watcher;
#ifndef __MINGW32__
static struct ev_signal sigchld_watcher;
#else
static struct plugin_watcher_t
{
	ev_io    io;
	SOCKET   fd;
	uint16_t port;
	int      valid;
} plugin_watcher;
#endif

#ifndef __MINGW32__
static int setnonblocking(int fd)
{
	int flags;
	if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
		flags = 0;
	}
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#endif

static int balloc(buffer_t *ptr, size_t capacity)
{
	memset(ptr, 0, sizeof(buffer_t));
	ptr->data     = ss_malloc(capacity);
	ptr->capacity = capacity;
	return capacity;
}

void bfree(buffer_t *ptr)
{
	if (ptr == NULL)
		return;
	ptr->idx      = 0;
	ptr->len      = 0;
	ptr->capacity = 0;
	if (ptr->data != NULL) {
		ss_free(ptr->data);
	}
}

int create_and_bind(const char *addr, const char *port)
{
	struct addrinfo  hints;
	struct addrinfo *result, *rp;
	int              s, listen_sock = -1;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family   = AF_UNSPEC;   /* Return IPv4 and IPv6 choices */
	hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */

	result = NULL;

	s = getaddrinfo(addr, port, &hints, &result);
	if (s != 0) {
		LOGI("getaddrinfo: %s", gai_strerror(s));
		return -1;
	}

	if (result == NULL) {
		LOGE("Could not bind");
		return -1;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		listen_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (listen_sock == -1) {
			continue;
		}

		int opt = 1;
		setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
		setsockopt(listen_sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
		if (reuse_port) {
			int err = set_reuseport(listen_sock);
			if (err == 0) {
				LOGI("tcp port reuse enabled");
			}
		}

		s = bind(listen_sock, rp->ai_addr, rp->ai_addrlen);
		if (s == 0) {
			/* We managed to bind successfully! */
			break;
		} else {
			ERROR("bind");
		}

		close(listen_sock);
		listen_sock = -1;
	}

	freeaddrinfo(result);

	return listen_sock;
}

static void server_recv_cb(EV_P_ ev_io *w, int revents)
{
	server_ctx_t *server_recv_ctx = (server_ctx_t *) w;
	server_t     *server          = server_recv_ctx->server;
	remote_t     *remote          = server->remote;

	if (remote == NULL) {
		close_and_free_server(EV_A_ server);
		return;
	}

	ssize_t r = recv(server->fd, remote->buf->data, SOCKET_BUF_SIZE, 0);

	if (r == 0) {
		// connection closed
		close_and_free_remote(EV_A_ remote);
		close_and_free_server(EV_A_ server);
		return;
	} else if (r == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// no data
			// continue to wait for recv
			return;
		} else {
			ERROR("server recv");
			close_and_free_remote(EV_A_ remote);
			close_and_free_server(EV_A_ server);
			return;
		}
	}

	remote->buf->len = r;

#ifdef XXXX // 加密 2024年12月02日 13:24:43
	int err = crypto->encrypt(remote->buf, server->e_ctx, SOCKET_BUF_SIZE);
#else
	int err = 0;
#endif

	if (err) {
		LOGE("invalid password or cipher");
		close_and_free_remote(EV_A_ remote);
		close_and_free_server(EV_A_ server);
		return;
	}

	int s = send(remote->fd, remote->buf->data, remote->buf->len, 0);

	if (s == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// no data, wait for send
			remote->buf->idx = 0;
			ev_io_stop(EV_A_ & server_recv_ctx->io);
			ev_io_start(EV_A_ & remote->send_ctx->io);
			return;
		} else {
			ERROR("send");
			close_and_free_remote(EV_A_ remote);
			close_and_free_server(EV_A_ server);
			return;
		}
	} else if (s < remote->buf->len) {
		remote->buf->len -= s;
		remote->buf->idx = s;
		ev_io_stop(EV_A_ & server_recv_ctx->io);
		ev_io_start(EV_A_ & remote->send_ctx->io);
		return;
	}
}

static void server_send_cb(EV_P_ ev_io *w, int revents)
{
	server_ctx_t *server_send_ctx = (server_ctx_t *) w;
	server_t     *server          = server_send_ctx->server;
	remote_t     *remote          = server->remote;
	if (server->buf->len == 0) {
		// close and free
		close_and_free_remote(EV_A_ remote);
		close_and_free_server(EV_A_ server);
		return;
	} else {
		// has data to send
		ssize_t s = send(server->fd, server->buf->data + server->buf->idx, server->buf->len, 0);
		if (s == -1) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				ERROR("send");
				close_and_free_remote(EV_A_ remote);
				close_and_free_server(EV_A_ server);
			}
			return;
		} else if (s < server->buf->len) {
			// partly sent, move memory, wait for the next time to send
			server->buf->len -= s;
			server->buf->idx += s;
			return;
		} else {
			// all sent out, wait for reading
			server->buf->len = 0;
			server->buf->idx = 0;
			ev_io_stop(EV_A_ & server_send_ctx->io);
			if (remote != NULL) {
				ev_io_start(EV_A_ & remote->recv_ctx->io);
			} else {
				close_and_free_remote(EV_A_ remote);
				close_and_free_server(EV_A_ server);
				return;
			}
		}
	}
}

static void remote_timeout_cb(EV_P_ ev_timer *watcher, int revents)
{
	remote_ctx_t *remote_ctx = cork_container_of(watcher, remote_ctx_t, watcher);

	remote_t *remote = remote_ctx->remote;
	server_t *server = remote->server;

	if (verbose) {
		LOGI("TCP connection timeout");
	}

	ev_timer_stop(EV_A_ watcher);

	close_and_free_remote(EV_A_ remote);
	close_and_free_server(EV_A_ server);
}

static void remote_recv_cb(EV_P_ ev_io *w, int revents)
{
	remote_ctx_t *remote_recv_ctx = (remote_ctx_t *) w;
	remote_t     *remote          = remote_recv_ctx->remote;
	server_t     *server          = remote->server;

	ssize_t r = recv(remote->fd, server->buf->data, SOCKET_BUF_SIZE, 0);

	if (r == 0) {
		// connection closed
		close_and_free_remote(EV_A_ remote);
		close_and_free_server(EV_A_ server);
		return;
	} else if (r == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// no data
			// continue to wait for recv
			return;
		} else {
			ERROR("remote recv");
			close_and_free_remote(EV_A_ remote);
			close_and_free_server(EV_A_ server);
			return;
		}
	}

	server->buf->len = r;

#ifdef XXXX // 解密 2024年12月02日 13:32:14
	int err = crypto->decrypt(server->buf, server->d_ctx, SOCKET_BUF_SIZE);

	if (err == CRYPTO_ERROR) {
		LOGE("invalid password or cipher");
		close_and_free_remote(EV_A_ remote);
		close_and_free_server(EV_A_ server);
		return;
	} else if (err == CRYPTO_NEED_MORE) {
		return; // Wait for more
	}
#endif

	int s = send(server->fd, server->buf->data, server->buf->len, 0);

	if (s == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// no data, wait for send
			server->buf->idx = 0;
			ev_io_stop(EV_A_ & remote_recv_ctx->io);
			ev_io_start(EV_A_ & server->send_ctx->io);
		} else {
			ERROR("send");
			close_and_free_remote(EV_A_ remote);
			close_and_free_server(EV_A_ server);
			return;
		}
	} else if (s < server->buf->len) {
		server->buf->len -= s;
		server->buf->idx = s;
		ev_io_stop(EV_A_ & remote_recv_ctx->io);
		ev_io_start(EV_A_ & server->send_ctx->io);
	}

	// Disable TCP_NODELAY after the first response are sent
	if (!remote->recv_ctx->connected && !no_delay) {
		int opt = 0;
		setsockopt(server->fd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
		setsockopt(remote->fd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
	}
	remote->recv_ctx->connected = 1;
}

static void remote_send_cb(EV_P_ ev_io *w, int revents)
{
	remote_ctx_t *remote_send_ctx = (remote_ctx_t *) w;
	remote_t     *remote          = remote_send_ctx->remote;
	server_t     *server          = remote->server;

	ev_timer_stop(EV_A_ & remote_send_ctx->watcher);

	if (!remote_send_ctx->connected) {
		int r = 0;

		if (remote->addr == NULL) {
			struct sockaddr_storage addr;
			socklen_t               len = sizeof(struct sockaddr_storage);
			r                           = getpeername(remote->fd, (struct sockaddr *) &addr, &len);
		}

		if (r == 0) {
			remote_send_ctx->connected = 1;

			assert(remote->buf->len == 0);
			buffer_t *abuf = remote->buf;

			ss_addr_t *sa = &server->destaddr;
#ifdef XXXX // 2024年12月02日 13:34:13
			struct cork_ip ip;
			if (cork_ip_init(&ip, sa->host) != -1) {
				if (ip.version == 4) {
					// send as IPv4
					struct in_addr host;
					memset(&host, 0, sizeof(struct in_addr));
					int host_len = sizeof(struct in_addr);

					if (inet_pton(AF_INET, sa->host, &host) == -1) {
						FATAL("IP parser error");
					}
					abuf->data[abuf->len++] = 1;
					memcpy(abuf->data + abuf->len, &host, host_len);
					abuf->len += host_len;
				} else if (ip.version == 6) {
					// send as IPv6
					struct in6_addr host;
					memset(&host, 0, sizeof(struct in6_addr));
					int host_len = sizeof(struct in6_addr);

					if (inet_pton(AF_INET6, sa->host, &host) == -1) {
						FATAL("IP parser error");
					}
					abuf->data[abuf->len++] = 4;
					memcpy(abuf->data + abuf->len, &host, host_len);
					abuf->len += host_len;
				} else {
					FATAL("IP parser error");
				}
#else
			if (1) {
				// send as IPv4
				struct in_addr host;
				memset(&host, 0, sizeof(struct in_addr));
				int host_len = sizeof(struct in_addr);

				if (inet_pton(AF_INET, sa->host, &host) == -1) {
					FATAL("IP parser error");
				}
				abuf->data[abuf->len++] = 1;
				memcpy(abuf->data + abuf->len, &host, host_len);
				abuf->len += host_len;
#endif
			} else {
				// send as domain
				int host_len = strlen(sa->host);

				abuf->data[abuf->len++] = 3;
				abuf->data[abuf->len++] = host_len;
				memcpy(abuf->data + abuf->len, sa->host, host_len);
				abuf->len += host_len;
			}

			uint16_t port = htons(atoi(sa->port));
			memcpy(abuf->data + abuf->len, &port, 2);
			abuf->len += 2;

#ifdef XXXX // 加密 2024年12月02日 13:35:07
			int err = crypto->encrypt(abuf, server->e_ctx, SOCKET_BUF_SIZE);
#else
			int err = 0;
#endif

			if (err) {
				LOGE("invalid password or cipher");
				close_and_free_remote(EV_A_ remote);
				close_and_free_server(EV_A_ server);
				return;
			}

			ev_io_start(EV_A_ & remote->recv_ctx->io);
		} else {
			ERROR("getpeername");
			// not connected
			close_and_free_remote(EV_A_ remote);
			close_and_free_server(EV_A_ server);
			return;
		}
	}

	if (remote->buf->len == 0) {
		// close and free
		close_and_free_remote(EV_A_ remote);
		close_and_free_server(EV_A_ server);
		return;
	} else {
		// has data to send
		ssize_t s = -1;
		if (remote->addr != NULL) {
#if defined(TCP_FASTOPEN_WINSOCK)
			DWORD s   = -1;
			DWORD err = 0;
			do {
				int optval = 1;
				// Set fast open option
				if (setsockopt(remote->fd, IPPROTO_TCP, TCP_FASTOPEN, &optval, sizeof(optval)) != 0) {
					ERROR("setsockopt");
					break;
				}
				// Load ConnectEx function
				LPFN_CONNECTEX ConnectEx = winsock_getconnectex();
				if (ConnectEx == NULL) {
					LOGE("Cannot load ConnectEx() function");
					err = WSAENOPROTOOPT;
					break;
				}
				// ConnectEx requires a bound socket
				if (winsock_dummybind(remote->fd, (struct sockaddr *) &(remote->addr)) != 0) {
					ERROR("bind");
					break;
				}
				// Call ConnectEx to send data
				memset(&remote->olap, 0, sizeof(remote->olap));
				remote->connect_ex_done = 0;
				if (ConnectEx(remote->fd,
				              (const struct sockaddr *) &(remote->addr),
				              get_sockaddr_len(remote->addr),
				              remote->buf->data,
				              remote->buf->len,
				              &s,
				              &remote->olap)) {
					remote->connect_ex_done = 1;
					break;
				}
				// XXX: ConnectEx pending, check later in remote_send
				if (WSAGetLastError() == ERROR_IO_PENDING) {
					err = CONNECT_IN_PROGRESS;
					break;
				}
				ERROR("ConnectEx");
			} while (0);
			// Set error number
			if (err) {
				SetLastError(err);
			}
#elif defined(CONNECT_DATA_IDEMPOTENT)
			((struct sockaddr_in *) &(remote->addr))->sin_len = sizeof(struct sockaddr_in);
			sa_endpoints_t endpoints;
			memset((char *) &endpoints, 0, sizeof(endpoints));
			endpoints.sae_dstaddr    = (struct sockaddr *) &(remote->addr);
			endpoints.sae_dstaddrlen = get_sockaddr_len(remote->addr);
			s = connectx(remote->fd, &endpoints, SAE_ASSOCID_ANY, CONNECT_RESUME_ON_READ_WRITE | CONNECT_DATA_IDEMPOTENT, NULL, 0, NULL, NULL);
#elif defined(TCP_FASTOPEN_CONNECT)
		int optval = 1;
		if (setsockopt(remote->fd, IPPROTO_TCP, TCP_FASTOPEN_CONNECT, (void *) &optval, sizeof(optval)) < 0)
			FATAL("failed to set TCP_FASTOPEN_CONNECT");
		s = connect(remote->fd, remote->addr, get_sockaddr_len(remote->addr));
		if (s == 0)
			s = send(remote->fd, remote->buf->data, remote->buf->len, 0);
#elif defined(MSG_FASTOPEN)
		s = sendto(remote->fd, remote->buf->data + remote->buf->idx, remote->buf->len, MSG_FASTOPEN, remote->addr, get_sockaddr_len(remote->addr));
#else
		FATAL("tcp fast open is not supported on this platform");
#endif

			remote->addr = NULL;

			if (s == -1) {
				if (errno == CONNECT_IN_PROGRESS) {
					ev_io_start(EV_A_ & remote_send_ctx->io);
					ev_timer_start(EV_A_ & remote_send_ctx->watcher);
				} else {
					fast_open = 0;
					if (errno == EOPNOTSUPP || errno == EPROTONOSUPPORT || errno == ENOPROTOOPT) {
						LOGE("fast open is not supported on this platform");
					} else {
						ERROR("fast_open_connect");
					}
					close_and_free_remote(EV_A_ remote);
					close_and_free_server(EV_A_ server);
				}
				return;
			}
		} else {
			s = send(remote->fd, remote->buf->data + remote->buf->idx, remote->buf->len, 0);
		}

		if (s == -1) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				ERROR("send");
				// close and free
				close_and_free_remote(EV_A_ remote);
				close_and_free_server(EV_A_ server);
			}
			return;
		} else if (s < remote->buf->len) {
			// partly sent, move memory, wait for the next time to send
			remote->buf->len -= s;
			remote->buf->idx += s;
			return;
		} else {
			// all sent out, wait for reading
			remote->buf->len = 0;
			remote->buf->idx = 0;
			ev_io_stop(EV_A_ & remote_send_ctx->io);
			ev_io_start(EV_A_ & server->recv_ctx->io);
		}
	}
}

static remote_t *new_remote(int fd, int timeout)
{
	remote_t *remote = ss_malloc(sizeof(remote_t));
	memset(remote, 0, sizeof(remote_t));

	remote->recv_ctx = ss_malloc(sizeof(remote_ctx_t));
	remote->send_ctx = ss_malloc(sizeof(remote_ctx_t));
	remote->buf      = ss_malloc(sizeof(buffer_t));
	balloc(remote->buf, SOCKET_BUF_SIZE);
	memset(remote->recv_ctx, 0, sizeof(remote_ctx_t));
	memset(remote->send_ctx, 0, sizeof(remote_ctx_t));
	remote->fd                  = fd;
	remote->recv_ctx->remote    = remote;
	remote->recv_ctx->connected = 0;
	remote->send_ctx->remote    = remote;
	remote->send_ctx->connected = 0;

	ev_io_init(&remote->recv_ctx->io, remote_recv_cb, fd, EV_READ);
	ev_io_init(&remote->send_ctx->io, remote_send_cb, fd, EV_WRITE);
	ev_timer_init(&remote->send_ctx->watcher, remote_timeout_cb, min(MAX_CONNECT_TIMEOUT, timeout), 0);

	return remote;
}

static void free_remote(remote_t *remote)
{
	if (remote->server != NULL) {
		remote->server->remote = NULL;
	}
	if (remote->buf != NULL) {
		bfree(remote->buf);
		ss_free(remote->buf);
	}
	ss_free(remote->recv_ctx);
	ss_free(remote->send_ctx);
	ss_free(remote);
}

static void close_and_free_remote(EV_P_ remote_t *remote)
{
	if (remote != NULL) {
		ev_timer_stop(EV_A_ & remote->send_ctx->watcher);
		ev_io_stop(EV_A_ & remote->send_ctx->io);
		ev_io_stop(EV_A_ & remote->recv_ctx->io);
		close(remote->fd);
		free_remote(remote);
	}
}

static server_t *new_server(int fd)
{
	server_t *server = ss_malloc(sizeof(server_t));
	memset(server, 0, sizeof(server_t));

	server->recv_ctx = ss_malloc(sizeof(server_ctx_t));
	server->send_ctx = ss_malloc(sizeof(server_ctx_t));
	server->buf      = ss_malloc(sizeof(buffer_t));
	balloc(server->buf, SOCKET_BUF_SIZE);
	memset(server->recv_ctx, 0, sizeof(server_ctx_t));
	memset(server->send_ctx, 0, sizeof(server_ctx_t));
	server->fd                  = fd;
	server->recv_ctx->server    = server;
	server->recv_ctx->connected = 0;
	server->send_ctx->server    = server;
	server->send_ctx->connected = 0;

#ifdef XXXX // 2024年12月02日 13:39:33
	server->e_ctx = ss_malloc(sizeof(cipher_ctx_t));
	server->d_ctx = ss_malloc(sizeof(cipher_ctx_t));
	crypto->ctx_init(crypto->cipher, server->e_ctx, 1);
	crypto->ctx_init(crypto->cipher, server->d_ctx, 0);
#endif

	ev_io_init(&server->recv_ctx->io, server_recv_cb, fd, EV_READ);
	ev_io_init(&server->send_ctx->io, server_send_cb, fd, EV_WRITE);

	return server;
}

static void free_server(server_t *server)
{
	if (server->remote != NULL) {
		server->remote->server = NULL;
	}
#ifdef XXXX // 2024年12月02日 13:39:43
	if (server->e_ctx != NULL) {
		crypto->ctx_release(server->e_ctx);
		ss_free(server->e_ctx);
	}
	if (server->d_ctx != NULL) {
		crypto->ctx_release(server->d_ctx);
		ss_free(server->d_ctx);
	}
#endif
	if (server->buf != NULL) {
		bfree(server->buf);
		ss_free(server->buf);
	}
	ss_free(server->recv_ctx);
	ss_free(server->send_ctx);
	ss_free(server);
}

static void close_and_free_server(EV_P_ server_t *server)
{
	if (server != NULL) {
		ev_io_stop(EV_A_ & server->send_ctx->io);
		ev_io_stop(EV_A_ & server->recv_ctx->io);
		close(server->fd);
		free_server(server);
	}
}

static void accept_cb(EV_P_ ev_io *w, int revents)
{
	struct listen_ctx *listener = (struct listen_ctx *) w;
	int                serverfd = accept(listener->fd, NULL, NULL);
	if (serverfd == -1) {
		ERROR("accept");
		return;
	}
	setnonblocking(serverfd);
	int opt = 1;
	setsockopt(serverfd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
	setsockopt(serverfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

	if (tcp_incoming_sndbuf > 0) {
		setsockopt(serverfd, SOL_SOCKET, SO_SNDBUF, &tcp_incoming_sndbuf, sizeof(int));
	}

	if (tcp_incoming_rcvbuf > 0) {
		setsockopt(serverfd, SOL_SOCKET, SO_RCVBUF, &tcp_incoming_rcvbuf, sizeof(int));
	}

	int              index       = rand() % listener->remote_num;
	struct sockaddr *remote_addr = listener->remote_addr[index];

	int protocol = IPPROTO_TCP;
	if (listener->mptcp < 0) {
		protocol = IPPROTO_MPTCP; // Enable upstream MPTCP
	}
	int remotefd = socket(remote_addr->sa_family, SOCK_STREAM, protocol);
	if (remotefd == -1) {
		ERROR("socket");
		return;
	}

#ifdef __ANDROID__
	if (vpn) {
		int not_protect = 0;
		if (remote_addr->sa_family == AF_INET) {
			struct sockaddr_in *s = (struct sockaddr_in *) remote_addr;
			if (s->sin_addr.s_addr == inet_addr("127.0.0.1"))
				not_protect = 1;
		}
		if (!not_protect) {
			if (protect_socket(remotefd) == -1) {
				ERROR("protect_socket");
				close(remotefd);
				return;
			}
		}
	}
#endif

	int keepAlive = 1;
	setsockopt(remotefd, SOL_SOCKET, SO_KEEPALIVE, (void *) &keepAlive, sizeof(keepAlive));
	setsockopt(remotefd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
	setsockopt(remotefd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

	// Enable out-of-tree MPTCP
	if (listener->mptcp > 1) {
		int err = setsockopt(remotefd, SOL_TCP, listener->mptcp, &opt, sizeof(opt));
		if (err == -1) {
			ERROR("failed to enable out-of-tree multipath TCP");
		}
	} else if (listener->mptcp == 1) {
		int i = 0;
		while ((listener->mptcp = mptcp_enabled_values[i]) > 0) {
			int err = setsockopt(remotefd, SOL_TCP, listener->mptcp, &opt, sizeof(opt));
			if (err != -1) {
				break;
			}
			i++;
		}
		if (listener->mptcp == 0) {
			ERROR("failed to enable out-of-tree multipath TCP");
		}
	}

	if (tcp_outgoing_sndbuf > 0) {
		setsockopt(remotefd, SOL_SOCKET, SO_SNDBUF, &tcp_outgoing_sndbuf, sizeof(int));
	}

	if (tcp_outgoing_rcvbuf > 0) {
		setsockopt(remotefd, SOL_SOCKET, SO_RCVBUF, &tcp_outgoing_rcvbuf, sizeof(int));
	}

	// Setup
	setnonblocking(remotefd);
#ifdef SET_INTERFACE
	if (listener->iface) {
		if (setinterface(remotefd, listener->iface) == -1)
			ERROR("setinterface");
	}
#endif

	server_t *server = new_server(serverfd);
	remote_t *remote = new_remote(remotefd, listener->timeout);
	server->destaddr = listener->tunnel_addr;
	server->remote   = remote;
	remote->server   = server;

	if (fast_open) {
		remote->addr = remote_addr;
	} else {
		int r = connect(remotefd, remote_addr, get_sockaddr_len(remote_addr));

		if (r == -1 && errno != CONNECT_IN_PROGRESS) {
			ERROR("connect");
			close_and_free_remote(EV_A_ remote);
			close_and_free_server(EV_A_ server);
			return;
		}
	}

	// listen to remote connected event
	ev_io_start(EV_A_ & remote->send_ctx->io);
	ev_timer_start(EV_A_ & remote->send_ctx->watcher);
}

static void signal_cb(EV_P_ ev_signal *w, int revents)
{
	if (revents & EV_SIGNAL) {
		switch (w->signum) {
#ifndef __MINGW32__
		case SIGCHLD:
			return;
#endif
		case SIGINT:
		case SIGTERM:
			ev_signal_stop(EV_DEFAULT, &sigint_watcher);
			ev_signal_stop(EV_DEFAULT, &sigterm_watcher);
#ifndef __MINGW32__
			ev_signal_stop(EV_DEFAULT, &sigchld_watcher);
#else
			ev_io_stop(EV_DEFAULT, &plugin_watcher.io);
#endif
			ev_unloop(EV_A_ EVUNLOOP_ALL);
		}
	}
}

#ifdef __MINGW32__
static void plugin_watcher_cb(EV_P_ ev_io *w, int revents)
{
	char   buf[1];
	SOCKET fd = accept(plugin_watcher.fd, NULL, NULL);
	if (fd == INVALID_SOCKET) {
		return;
	}
	recv(fd, buf, 1, 0);
	closesocket(fd);
	LOGE("plugin service exit unexpectedly");
	ret_val = -1;
	ev_signal_stop(EV_DEFAULT, &sigint_watcher);
	ev_signal_stop(EV_DEFAULT, &sigterm_watcher);
	ev_io_stop(EV_DEFAULT, &plugin_watcher.io);
	ev_unloop(EV_A_ EVUNLOOP_ALL);
}

#endif

int main(int argc, char **argv)
{
	srand(time(NULL));

	int   i, c;
	int   pid_flags  = 0;
	int   mptcp      = 0;
	int   mtu        = 0;
	char *user       = NULL;
	char *local_port = NULL;
	char *local_addr = NULL;
	char *password   = NULL;
	char *key        = NULL;
	char *timeout    = NULL;
	char *method     = NULL;
	char *pid_path   = NULL;
	char *iface      = NULL;
	char  tmp_port[8];

	ss_addr_t tunnel_addr     = {.host = NULL, .port = NULL};
	char     *tunnel_addr_str = NULL;

	int       remote_num  = 0;
	char     *remote_port = NULL;
	ss_addr_t remote_addr[MAX_REMOTE_NUM];

	memset(remote_addr, 0, sizeof(ss_addr_t) * MAX_REMOTE_NUM);

	static struct option long_options[] = {{"fast-open", no_argument, NULL, GETOPT_VAL_FAST_OPEN},
	                                       {"mtu", required_argument, NULL, GETOPT_VAL_MTU},
	                                       {"no-delay", no_argument, NULL, GETOPT_VAL_NODELAY},
	                                       {"mptcp", no_argument, NULL, GETOPT_VAL_MPTCP},
	                                       {"reuse-port", no_argument, NULL, GETOPT_VAL_REUSE_PORT},
	                                       {"tcp-incoming-sndbuf", required_argument, NULL, GETOPT_VAL_TCP_INCOMING_SNDBUF},
	                                       {"tcp-incoming-rcvbuf", required_argument, NULL, GETOPT_VAL_TCP_INCOMING_RCVBUF},
	                                       {"tcp-outgoing-sndbuf", required_argument, NULL, GETOPT_VAL_TCP_OUTGOING_SNDBUF},
	                                       {"tcp-outgoing-rcvbuf", required_argument, NULL, GETOPT_VAL_TCP_OUTGOING_RCVBUF},
	                                       {"password", required_argument, NULL, GETOPT_VAL_PASSWORD},
	                                       {"key", required_argument, NULL, GETOPT_VAL_KEY},
	                                       {"help", no_argument, NULL, GETOPT_VAL_HELP},
	                                       {NULL, 0, NULL, 0}};

	opterr = 0;

	USE_TTY();

#ifdef __ANDROID__
	while ((c = getopt_long(argc, argv, "f:s:p:l:k:t:m:i:c:b:L:a:n:huUvV6A", long_options, NULL)) != -1) {
#else
	while ((c = getopt_long(argc, argv, "f:s:p:l:k:t:m:i:c:b:L:a:n:huUv6A", long_options, NULL)) != -1) {
#endif
		switch (c) {
		case GETOPT_VAL_FAST_OPEN:
			fast_open = 1;
			break;
		case GETOPT_VAL_MTU:
			mtu = atoi(optarg);
			LOGI("set MTU to %d", mtu);
			break;
		case GETOPT_VAL_MPTCP:
			mptcp = get_mptcp(1);
			if (mptcp)
				LOGI("enable multipath TCP (%s)", mptcp > 0 ? "out-of-tree" : "upstream");
			break;
		case GETOPT_VAL_NODELAY:
			no_delay = 1;
			LOGI("enable TCP no-delay");
			break;
		case GETOPT_VAL_KEY:
			key = optarg;
			break;
		case GETOPT_VAL_REUSE_PORT:
			reuse_port = 1;
			break;
		case GETOPT_VAL_TCP_INCOMING_SNDBUF:
			tcp_incoming_sndbuf = atoi(optarg);
			break;
		case GETOPT_VAL_TCP_INCOMING_RCVBUF:
			tcp_incoming_rcvbuf = atoi(optarg);
			break;
		case GETOPT_VAL_TCP_OUTGOING_SNDBUF:
			tcp_outgoing_sndbuf = atoi(optarg);
			break;
		case GETOPT_VAL_TCP_OUTGOING_RCVBUF:
			tcp_outgoing_rcvbuf = atoi(optarg);
			break;
		case 's':
			if (remote_num < MAX_REMOTE_NUM) {
				parse_addr(optarg, &remote_addr[remote_num++]);
			}
			break;
		case 'p':
			remote_port = optarg;
			break;
		case 'l':
			local_port = optarg;
			break;
		case GETOPT_VAL_PASSWORD:
		case 'k':
			password = optarg;
			break;
		case 'f':
			pid_flags = 1;
			pid_path  = optarg;
			break;
		case 't':
			timeout = optarg;
			break;
		case 'm':
			method = optarg;
			break;
		case 'i':
			iface = optarg;
			break;
		case 'b':
			local_addr = optarg;
			break;
		case 'u':
			mode = TCP_AND_UDP;
			break;
		case 'U':
			mode = UDP_ONLY;
			break;
		case 'L':
			tunnel_addr_str = optarg;
			break;
		case 'a':
			user = optarg;
			break;
#ifdef HAVE_SETRLIMIT
		case 'n':
			nofile = atoi(optarg);
			break;
#endif
		case 'v':
			verbose = 1;
			break;
		case GETOPT_VAL_HELP:
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		case '6':
			ipv6first = 1;
			break;
#ifdef __ANDROID__
		case 'V':
			vpn = 1;
			break;
#endif
		case 'A':
			FATAL("One time auth has been deprecated. Try AEAD ciphers instead.");
			break;
		case '?':
			// The option character is not recognized.
			LOGE("Unrecognized option: %s", optarg);
			opterr = 1;
			break;
		}
	}

	if (opterr) {
		usage();
		exit(EXIT_FAILURE);
	}

	if (remote_num == 0 || remote_port == NULL || tunnel_addr_str == NULL || local_port == NULL || (password == NULL && key == NULL)) {
		usage();
		exit(EXIT_FAILURE);
	}

#ifdef __MINGW32__
	winsock_init();
#endif

	if (tcp_incoming_sndbuf != 0 && tcp_incoming_sndbuf < SOCKET_BUF_SIZE) {
		tcp_incoming_sndbuf = 0;
	}

	if (tcp_incoming_sndbuf != 0) {
		LOGI("set TCP incoming connection send buffer size to %d", tcp_incoming_sndbuf);
	}

	if (tcp_incoming_rcvbuf != 0 && tcp_incoming_rcvbuf < SOCKET_BUF_SIZE) {
		tcp_incoming_rcvbuf = 0;
	}

	if (tcp_incoming_rcvbuf != 0) {
		LOGI("set TCP incoming connection receive buffer size to %d", tcp_incoming_rcvbuf);
	}

	if (tcp_outgoing_sndbuf != 0 && tcp_outgoing_sndbuf < SOCKET_BUF_SIZE) {
		tcp_outgoing_sndbuf = 0;
	}

	if (tcp_outgoing_sndbuf != 0) {
		LOGI("set TCP outgoing connection send buffer size to %d", tcp_outgoing_sndbuf);
	}

	if (tcp_outgoing_rcvbuf != 0 && tcp_outgoing_rcvbuf < SOCKET_BUF_SIZE) {
		tcp_outgoing_rcvbuf = 0;
	}

	if (tcp_outgoing_rcvbuf != 0) {
		LOGI("set TCP outgoing connection receive buffer size to %d", tcp_outgoing_rcvbuf);
	}

	if (method == NULL) {
		method = "chacha20-ietf-poly1305";
	}

	if (timeout == NULL) {
		timeout = "60";
	}

#ifdef HAVE_SETRLIMIT
	/*
     * no need to check the return value here since we will show
     * the user an error message if setrlimit(2) fails
     */
	if (nofile > 1024) {
		if (verbose) {
			LOGI("setting NOFILE to %d", nofile);
		}
		set_nofile(nofile);
	}
#endif

	if (local_addr == NULL) {
		if (is_ipv6only(remote_addr, remote_num, ipv6first)) {
			local_addr = "::1";
		} else {
			local_addr = "127.0.0.1";
		}
	}

	if (fast_open == 1) {
#ifdef TCP_FASTOPEN
		LOGI("using tcp fast open");
#else
		LOGE("tcp fast open is not supported by this environment");
		fast_open = 0;
#endif
	}

	USE_SYSLOG(argv[0], pid_flags);
	if (pid_flags) {
		daemonize(pid_path);
	}

	if (ipv6first) {
		LOGI("resolving hostname to IPv6 address first");
	}

	// parse tunnel addr
	parse_addr(tunnel_addr_str, &tunnel_addr);

	if (tunnel_addr.port == NULL) {
		FATAL("tunnel port is not defined");
	}

#ifdef __MINGW32__
	// Listen on plugin control port
	if (plugin != NULL && plugin_watcher.port != 0) {
		SOCKET fd;
		fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd != INVALID_SOCKET) {
			plugin_watcher.valid = 0;
			do {
				struct sockaddr_in addr;
				memset(&addr, 0, sizeof(addr));
				addr.sin_family      = AF_INET;
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				addr.sin_port        = htons(plugin_watcher.port);
				if (bind(fd, (struct sockaddr *) &addr, sizeof(addr))) {
					LOGE("failed to bind plugin control port");
					break;
				}
				if (listen(fd, 1)) {
					LOGE("failed to listen on plugin control port");
					break;
				}
				plugin_watcher.fd = fd;
				ev_io_init(&plugin_watcher.io, plugin_watcher_cb, fd, EV_READ);
				ev_io_start(EV_DEFAULT, &plugin_watcher.io);
				plugin_watcher.valid = 1;
			} while (0);
			if (!plugin_watcher.valid) {
				closesocket(fd);
				plugin_watcher.port = 0;
			}
		}
	}
#endif

#ifndef __MINGW32__
	// ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);
	signal(SIGABRT, SIG_IGN);
#endif

	ev_signal_init(&sigint_watcher, signal_cb, SIGINT);
	ev_signal_init(&sigterm_watcher, signal_cb, SIGTERM);
	ev_signal_start(EV_DEFAULT, &sigint_watcher);
	ev_signal_start(EV_DEFAULT, &sigterm_watcher);
#ifndef __MINGW32__
	ev_signal_init(&sigchld_watcher, signal_cb, SIGCHLD);
	ev_signal_start(EV_DEFAULT, &sigchld_watcher);
#endif

	// Setup keys
	LOGI("initializing ciphers... %s", method);
#ifdef XXXX // 2024年12月02日 13:41:52
	crypto = crypto_init(password, key, method);
	if (crypto == NULL)
		FATAL("failed to initialize ciphers");
#endif

	// Setup proxy context
	struct listen_ctx listen_ctx;
	memset(&listen_ctx, 0, sizeof(struct listen_ctx));
	listen_ctx.tunnel_addr = tunnel_addr;
	listen_ctx.remote_num  = remote_num;
	listen_ctx.remote_addr = ss_malloc(sizeof(struct sockaddr *) * remote_num);
	memset(listen_ctx.remote_addr, 0, sizeof(struct sockaddr *) * remote_num);
	for (i = 0; i < remote_num; i++) {
		char *host = remote_addr[i].host;
		char *port = remote_addr[i].port == NULL ? remote_port : remote_addr[i].port;
		struct sockaddr_storage *storage = ss_malloc(sizeof(struct sockaddr_storage));
		memset(storage, 0, sizeof(struct sockaddr_storage));
		if (get_sockaddr(host, port, storage, 1, ipv6first) == -1) {
			FATAL("failed to resolve the provided hostname");
		}
		listen_ctx.remote_addr[i] = (struct sockaddr *) storage;
	}
	listen_ctx.timeout = atoi(timeout);
	listen_ctx.iface   = iface;
	listen_ctx.mptcp   = mptcp;

	LOGI("listening at %s:%s", local_addr, local_port);

	struct ev_loop *loop = EV_DEFAULT;

	if (mode != UDP_ONLY) {
		// Setup socket
		int listenfd;
		listenfd = create_and_bind(local_addr, local_port);
		if (listenfd == -1) {
			FATAL("bind() error");
		}
		if (listen(listenfd, SOMAXCONN) == -1) {
			FATAL("listen() error");
		}
		setnonblocking(listenfd);

		listen_ctx.fd = listenfd;

		ev_io_init(&listen_ctx.io, accept_cb, listenfd, EV_READ);
		ev_io_start(loop, &listen_ctx.io);
	}

	// Setup UDP
	if (mode != TCP_ONLY) {
		LOGI("UDP relay enabled");
		char                    *host    = remote_addr[0].host;
		char                    *port    = remote_addr[0].port == NULL ? remote_port : remote_addr[0].port;
		struct sockaddr_storage *storage = ss_malloc(sizeof(struct sockaddr_storage));
		memset(storage, 0, sizeof(struct sockaddr_storage));
		if (get_sockaddr(host, port, storage, 1, ipv6first) == -1) {
			FATAL("failed to resolve the provided hostname");
		}
		struct sockaddr *addr = (struct sockaddr *) storage;
		init_udprelay(local_addr, local_port, addr, get_sockaddr_len(addr), tunnel_addr, mtu, listen_ctx.timeout, iface);
	}

	if (mode == UDP_ONLY) {
		LOGI("TCP relay disabled");
	}

#ifndef __MINGW32__
	// setuid
	if (user != NULL && !run_as(user)) {
		FATAL("failed to switch user");
	}

	if (geteuid() == 0) {
		LOGI("running from root user");
	}
#endif

	ev_run(loop, 0);

	for (i = 0; i < remote_num; i++)
		ss_free(listen_ctx.remote_addr[i]);
	ss_free(listen_ctx.remote_addr);

#ifdef __MINGW32__
	if (plugin_watcher.valid) {
		closesocket(plugin_watcher.fd);
	}

	winsock_cleanup();
#endif

	return ret_val;
}