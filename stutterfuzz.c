#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "list.h"
#include "rand.h"

#pragma GCC diagnostic ignored "-Wvla"

static struct {
	char *blob_dir;
	char *node;
	char *service;
	uint32_t num_conns;
	uint32_t cycle_ms;
	uint32_t fastopen_chance;
	uint32_t close_chance;
} config = {
	.num_conns = 100,
	.cycle_ms = 150,
	.fastopen_chance = 2,
	.close_chance = 500,
};

struct file {
	char *path;
	char *buf;
	size_t len;
	struct list_head file_list;
};

struct conn {
	int fd;
	struct file *file;
	uint64_t start_cycle;
	size_t offset;
	struct list_head conn_list;
};

static struct list_head file_head = LIST_HEAD_INIT(file_head);
static struct list_head conn_open_head = LIST_HEAD_INIT(conn_open_head);
static struct list_head conn_pending_head = LIST_HEAD_INIT(conn_pending_head);

static int epoll_fd = -1;
static struct addrinfo *addrs = NULL;
static uint64_t rounds = 0, open_conns = 0, cycle = 0;
static bool shutdown_flag = false;
static double mean_cycles_to_connect = 1.0;
static uint64_t conn_send_ready = 0, conn_send_not_ready = 0;

#define CYCLE_SMOOTHING 0.9999

static void do_shutdown(int __attribute__((unused)) signal) {
	shutdown_flag = true;
}

static bool parse_opts(int argc, char *argv[]) {
	static struct option long_options[] = {
		{"blob-dir",        required_argument, 0, 'b'},
		{"host",            required_argument, 0, 'h'},
		{"port",            required_argument, 0, 'p'},
		{"cycle-ms",        required_argument, 0, 'c'},
		{"num-conns",       required_argument, 0, 'n'},
		{"fastopen-chance", required_argument, 0, 'f'},
		{"close-chance",    required_argument, 0, 'l'},
		{0,                 0,                 0, 0  },
	};

	int opt;
	while ((opt = getopt_long_only(argc, argv, "", long_options, NULL)) != -1) {
		switch (opt) {
			case 'b':
				config.blob_dir = optarg;
				break;

			case 'h':
				config.node = optarg;
				break;

			case 'p':
				config.service = optarg;
				break;

			case 'c':
				config.cycle_ms = (uint32_t) strtoul(optarg, NULL, 10);
				break;

			case 'n':
				config.num_conns = (uint32_t) strtoul(optarg, NULL, 10);
				assert(config.num_conns);
				break;

			case 'f':
				config.fastopen_chance = (uint32_t) strtoul(optarg, NULL, 10);
				break;

			case 'l':
				config.close_chance = (uint32_t) strtoul(optarg, NULL, 10);
				break;

			default:
				return false;
		}
	}

	if (optind != argc) {
		return false;
	}

	if (!config.blob_dir ||
			!config.node ||
			!config.service) {
		return false;
	}

	return true;
}

static void stats_print() {
	fprintf(stderr, "\rStats: rounds=%ju, mean_cycles_to_connect=%0.2f, ready_to_send=%0.2f            \r",
			(uintmax_t) rounds,
			mean_cycles_to_connect,
			(double) conn_send_ready / (double) (conn_send_ready + conn_send_not_ready));
}

static void file_open() {
	size_t dirlen = strlen(config.blob_dir);

	DIR *dir = opendir(config.blob_dir);
	assert(dir);

	uint64_t max_size = 0, min_size = UINT64_MAX, total_size = 0, num_blobs = 0;

	while (true) {
		struct dirent entry, *ret;
		assert(!readdir_r(dir, &entry, &ret));
		if (!ret) {
			break;
		}
		if (entry.d_name[0] == '.') {
			continue;
		}

		struct file *file = malloc(sizeof(*file));
		assert(file);
		list_add(&file->file_list, &file_head);

		size_t max_len = dirlen + strlen(entry.d_name) + 2;
		file->path = malloc(max_len);
		assert(file->path);
		snprintf(file->path, max_len, "%s/%s", config.blob_dir, entry.d_name);

		int fd = open(file->path, O_RDONLY);
		assert(fd >= 0);

		struct stat stat;
		assert(!fstat(fd, &stat));

		file->len = (size_t) stat.st_size;

		max_size = max_size < file->len ? file->len : max_size;
		min_size = min_size > file->len ? file->len : min_size;
		num_blobs++;
		total_size += file->len;

		file->buf = mmap(NULL, file->len, PROT_READ, MAP_SHARED, fd, 0);
		assert(file->buf);

		assert(!close(fd));
	}

	assert(num_blobs);

	fprintf(stderr, "Loaded %ju blobs. Bytes: %ju min, %ju mean, %ju max\n",
			(uintmax_t) num_blobs,
			(uintmax_t) min_size,
			(uintmax_t) (total_size / num_blobs),
			(uintmax_t) max_size);

	assert(!closedir(dir));
}

static void file_del(struct file *file) {
	free(file->path);
	assert(!munmap(file->buf, file->len));
	list_del(&file->file_list);
	free(file);
}

static void file_cleanup() {
	struct file *iter, *next;
	list_for_each_entry_safe(iter, next, &file_head, file_list) {
		file_del(iter);
	}
}

static struct file *file_next() {
	static struct list_head *first = NULL, *iter;

	if (!first) {
		iter = first = file_head.next;
	}

	if (iter->next == &file_head) {
		iter = iter->next->next;
		++rounds;
	} else {
		iter = iter->next;
	}
	return list_entry(iter, struct file, file_list);
}

static size_t conn_get_split(struct conn *conn) {
	size_t total_len = conn->file->len;
	size_t remaining_len = total_len - conn->offset;
	size_t rnd;
	rand_fill(&rnd, sizeof(rnd));
	rnd = (rnd % total_len) + 1;
	return rnd > remaining_len ? remaining_len : rnd;
}

static void conn_new() {
	if (!addrs) {
		struct addrinfo hints = {
			.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG,
			.ai_family = AF_UNSPEC,
			.ai_socktype = SOCK_STREAM,
		};
		assert(!getaddrinfo(config.node, config.service, &hints, &addrs));
	}

	struct conn *conn = malloc(sizeof(*conn));
	assert(conn);
	conn->fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
	assert(conn->fd >= 0);

	conn->file = file_next();
	conn->offset = 0;
	conn->offset = rand_yes_no(config.fastopen_chance) ? conn_get_split(conn) : 0;
	sendto(conn->fd, conn->file->buf, conn->offset, MSG_DONTWAIT | MSG_FASTOPEN, addrs[0].ai_addr, addrs[0].ai_addrlen);
	conn->start_cycle = cycle;
	list_add(&conn->conn_list, &conn_pending_head);
	struct epoll_event ev = {
		.events = EPOLLOUT,
		.data = {
			.ptr = conn,
		},
	};
	assert(!epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev));
	open_conns++;
}

static void conn_del(struct conn *conn) {
	assert(!close(conn->fd));
	list_del(&conn->conn_list);
	free(conn);
	open_conns--;
}

static void conn_cleanup() {
	struct conn *iter, *next;
	list_for_each_entry_safe(iter, next, &conn_pending_head, conn_list) {
		conn_del(iter);
	}
	list_for_each_entry_safe(iter, next, &conn_open_head, conn_list) {
		conn_del(iter);
	}
}

static void conn_fill() {
	while (open_conns < config.num_conns) {
		conn_new();
	}
}

static bool conn_ready_to_send(struct conn *conn) {
	struct tcp_info tcp_info;
	socklen_t tcp_info_length = sizeof(tcp_info);
	assert(!getsockopt(conn->fd, SOL_TCP, TCP_INFO, &tcp_info, &tcp_info_length));
	return !tcp_info.tcpi_unacked;
}

static void conn_send_message(struct conn *conn) {
	if (conn_ready_to_send(conn)) {
		conn_send_ready++;
	} else {
		conn_send_not_ready++;
		return;
	}

	if (conn->offset == conn->file->len) {
		conn_del(conn);
		return;
	}

	if (rand_yes_no(config.close_chance)) {
		conn_del(conn);
		return;
	}

	size_t to_send = conn_get_split(conn);
	if (send(conn->fd, conn->file->buf + conn->offset, to_send, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t) to_send) {
		conn_del(conn);
		return;
	}
	conn->offset += to_send;
	if (conn->offset == conn->file->len) {
		conn_del(conn);
		return;
	}
}

static void conn_check(struct conn *conn) {
	int error;
  socklen_t len = sizeof(error);
	assert(getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0);
	if (error) {
		fprintf(stderr, "\nConnection failed: %s", strerror(error));
		shutdown_flag = true;
		return;
	}
	list_del(&conn->conn_list);
	list_add(&conn->conn_list, &conn_open_head);
	assert(!epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL));

	uint64_t cycles_to_connect = cycle - conn->start_cycle;
	mean_cycles_to_connect = (
			(mean_cycles_to_connect * CYCLE_SMOOTHING) +
			((double) cycles_to_connect * (1.0 - CYCLE_SMOOTHING)));
}

static void conn_check_all() {
	struct epoll_event evs[config.num_conns];
	int nfds = epoll_wait(epoll_fd, evs, (int) config.num_conns, 0);
	assert(nfds >= 0);
	for (int i = 0; i < nfds; i++) {
		struct conn *conn = evs[i].data.ptr;
		conn_check(conn);
	}
}

static void conn_cycle() {
	struct conn *iter, *next;
	list_for_each_entry_safe(iter, next, &conn_open_head, conn_list) {
		conn_send_message(iter);
	}
	conn_check_all();
}

int main(int argc, char *argv[]) {
	rand_init();

	if (!parse_opts(argc, argv)) {
		fprintf(stderr,
			"Usage: %s [OPTION]...\n"
			"\n"
			"Options:\n"
			"\t--blob-dir=PATH [required]\n"
			"\t--host=HOST [required]\n"
			"\t--port=PORT [required]\n"
			"\t--cycle-ms=CYCLE_MILLSECONDS [default 150]\n"
			"\t--num-conns=PARALLEL_CONNECTIONS [default 100]\n"
			"\t--fastopen-chance=CHANCE [default 2]\n"
			"\t--close-chance=CHANCE [default 500]\n"
			, argv[0]);
		exit(EXIT_FAILURE);
	}

	signal(SIGINT, do_shutdown);
	signal(SIGTERM, do_shutdown);
	assert(!close(STDIN_FILENO));
	assert(!close(STDOUT_FILENO));

	file_open();

	stats_print();

	epoll_fd = epoll_create1(0);
	assert(epoll_fd >= 0);

#define NS_PER_S 1000000000
#define MS_PER_S 1000
#define NS_PER_MS 1000000
	uint64_t cycle_ns = config.cycle_ms * NS_PER_MS;
	struct timespec ts = {
		.tv_sec = cycle_ns / NS_PER_S,
		.tv_nsec = (config.cycle_ms % MS_PER_S) * NS_PER_MS,
	};

	while (!shutdown_flag) {
		if (!(++cycle % 100)) {
			stats_print();
		}
		conn_cycle();
		conn_fill();
		nanosleep(&ts, NULL);
	}

	fprintf(stderr, "\n");

	conn_cleanup();
	file_cleanup();
	rand_cleanup();
	freeaddrinfo(addrs);

	assert(!close(epoll_fd));
	assert(!close(STDERR_FILENO));
}
