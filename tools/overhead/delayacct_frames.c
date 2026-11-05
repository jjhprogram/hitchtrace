// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * delayacct_frames: per-frame delay accounting, with no BPF.
 *
 * The comparator for "what can you get with no kernel code of your own?". A
 * uprobe (perf_event_open, no program attached) marks each frame; at each mark
 * it reads every thread's delay counters through taskstats and diffs them. A
 * frame over budget gets one line per thread that had any delay. There is no
 * futex category and no waker: the kernel's counters do not have them.
 *
 * Needs root (CAP_NET_ADMIN for taskstats, CAP_PERFMON for the uprobe) and
 * sysctl kernel.task_delayacct=1.
 *
 * usage: delayacct_frames PID LIB OFFSET BUDGET_US OUT
 *        OFFSET is the marker's file offset in LIB
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/perf_event.h>
#include <linux/taskstats.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX_CPUS	256
#define MAX_THREADS	512
#define RING_PAGES	8
#define REFRESH_FRAMES	60	/* look for new threads this often */

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* ------------------------------------------------------------ taskstats */

struct msg {
	struct nlmsghdr n;
	struct genlmsghdr g;
	char buf[2048];
};

#define GENLMSG_DATA(m)	((void *)((char *)NLMSG_DATA(m) + GENL_HDRLEN))
#define NLA_DATA(a)	((void *)((char *)(a) + NLA_HDRLEN))

static int nl_fd;
static __u16 family;

static int nl_send(__u16 type, __u8 cmd, __u16 attr, const void *data, __u16 len)
{
	struct msg m;
	struct nlattr *a;

	memset(&m, 0, sizeof(m));
	m.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	m.n.nlmsg_type = type;
	m.n.nlmsg_flags = NLM_F_REQUEST;
	m.n.nlmsg_pid = getpid();
	m.g.cmd = cmd;
	m.g.version = 1;
	a = GENLMSG_DATA(&m);
	a->nla_type = attr;
	a->nla_len = NLA_HDRLEN + len;
	memcpy(NLA_DATA(a), data, len);
	m.n.nlmsg_len += NLMSG_ALIGN(a->nla_len);
	return send(nl_fd, &m, m.n.nlmsg_len, 0) < 0 ? -1 : 0;
}

static int nl_open(void)
{
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
	struct msg r;
	struct nlattr *a;
	int len;

	nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (nl_fd < 0 || bind(nl_fd, (struct sockaddr *)&addr, sizeof(addr)))
		return -1;
	if (nl_send(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME,
		    TASKSTATS_GENL_NAME, strlen(TASKSTATS_GENL_NAME) + 1))
		return -1;
	len = recv(nl_fd, &r, sizeof(r), 0);
	if (len < 0 || r.n.nlmsg_type == NLMSG_ERROR)
		return -1;
	a = GENLMSG_DATA(&r);
	a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
	if (a->nla_type != CTRL_ATTR_FAMILY_ID)
		return -1;
	family = *(__u16 *)NLA_DATA(a);
	return 0;
}

/* The counters of @tid; nonzero if the thread is gone. */
static int task_stats(__u32 tid, struct taskstats *out)
{
	struct msg r;
	struct nlattr *a, *in;
	int len, left, inleft;

	if (nl_send(family, TASKSTATS_CMD_GET, TASKSTATS_CMD_ATTR_PID, &tid, sizeof(tid)))
		return -1;
	len = recv(nl_fd, &r, sizeof(r), 0);
	if (len < 0 || r.n.nlmsg_type == NLMSG_ERROR || !NLMSG_OK(&r.n, (unsigned)len))
		return -1;
	left = NLMSG_PAYLOAD(&r.n, 0) - GENL_HDRLEN;
	a = GENLMSG_DATA(&r);
	while (left > 0) {
		if (a->nla_type == TASKSTATS_TYPE_AGGR_PID) {
			inleft = a->nla_len - NLA_HDRLEN;
			in = NLA_DATA(a);
			while (inleft > 0) {
				if (in->nla_type == TASKSTATS_TYPE_STATS) {
					size_t n = in->nla_len - NLA_HDRLEN;

					if (n > sizeof(*out))
						n = sizeof(*out);
					memcpy(out, NLA_DATA(in), n);
					return 0;
				}
				inleft -= NLA_ALIGN(in->nla_len);
				in = (struct nlattr *)((char *)in + NLA_ALIGN(in->nla_len));
			}
		}
		left -= NLA_ALIGN(a->nla_len);
		a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
	}
	return -1;
}

/* ---------------------------------------------------------- the threads */

struct thread {
	__u32 tid;
	int have;			/* last holds a reading */
	struct taskstats last;
};

static struct thread threads[MAX_THREADS];
static int nthreads;

static void refresh_threads(int pid)
{
	char path[64];
	struct dirent *de;
	DIR *d;

	snprintf(path, sizeof(path), "/proc/%d/task", pid);
	d = opendir(path);
	if (!d)
		return;
	while ((de = readdir(d))) {
		__u32 tid = strtoul(de->d_name, NULL, 10);
		int i;

		if (!tid)
			continue;
		for (i = 0; i < nthreads; i++)
			if (threads[i].tid == tid)
				break;
		if (i == nthreads && nthreads < MAX_THREADS) {
			threads[nthreads].tid = tid;
			threads[nthreads].have = 0;
			nthreads++;
		}
	}
	closedir(d);
}

/* Read every thread; when @emit, write what changed since the last frame. */
static unsigned long read_frame(FILE *out, unsigned long long frame, unsigned long long end_ns,
				unsigned long long frame_ns, int emit)
{
	unsigned long lines = 0;

	for (int i = 0; i < nthreads; i++) {
		struct thread *t = &threads[i];
		struct taskstats s;

		memset(&s, 0, sizeof(s));
		if (task_stats(t->tid, &s))
			continue;
		if (emit && t->have) {
			unsigned long long cpu = s.cpu_delay_total - t->last.cpu_delay_total;
			unsigned long long io = s.blkio_delay_total - t->last.blkio_delay_total;
			unsigned long long swap = s.swapin_delay_total - t->last.swapin_delay_total;
			unsigned long long rec = s.freepages_delay_total - t->last.freepages_delay_total;
			unsigned long long comp = s.compact_delay_total - t->last.compact_delay_total;
			unsigned long long thr = s.thrashing_delay_total - t->last.thrashing_delay_total;

			if (cpu || io || swap || rec || comp || thr) {
				fprintf(out, "{\"frame\":%llu,\"end_ns\":%llu,\"frame_ns\":%llu,\"tid\":%u,\"comm\":\"%s\","
					"\"cpu_ns\":%llu,\"blkio_ns\":%llu,\"swapin_ns\":%llu,"
					"\"reclaim_ns\":%llu,\"compact_ns\":%llu,\"thrashing_ns\":%llu}\n",
					frame, end_ns, frame_ns, t->tid, s.ac_comm, cpu, io, swap, rec, comp, thr);
				lines++;
			}
		}
		t->last = s;
		t->have = 1;
	}
	return lines;
}

/* ------------------------------------------------------------ the uprobe */

struct ring {
	int fd;
	struct perf_event_mmap_page *meta;
	char *data;
	size_t size;
};

static int uprobe_type(void)
{
	FILE *f = fopen("/sys/bus/event_source/devices/uprobe/type", "r");
	int t = -1;

	if (f) {
		if (fscanf(f, "%d", &t) != 1)
			t = -1;
		fclose(f);
	}
	return t;
}

static int open_ring(struct ring *r, int type, const char *lib, unsigned long long offset, int cpu)
{
	struct perf_event_attr a;
	long page = sysconf(_SC_PAGESIZE);
	void *m;

	memset(&a, 0, sizeof(a));
	a.type = type;
	a.size = sizeof(a);
	a.config1 = (unsigned long long)(uintptr_t)lib;
	a.config2 = offset;
	a.sample_period = 1;
	a.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
	a.wakeup_events = 1;
	a.use_clockid = 1;
	a.clockid = CLOCK_MONOTONIC;
	r->fd = syscall(SYS_perf_event_open, &a, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
	if (r->fd < 0)
		return -1;
	r->size = page * RING_PAGES;
	m = mmap(NULL, page + r->size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
	if (m == MAP_FAILED)
		return -1;
	r->meta = m;
	r->data = (char *)m + page;
	return ioctl(r->fd, PERF_EVENT_IOC_ENABLE, 0);
}

struct sample {
	struct perf_event_header h;
	__u32 pid, tid;
	__u64 time;
};

/* The next sample of @pid in the ring, if there is one. */
static int next_sample(struct ring *r, int pid, __u64 *time, unsigned long *lost)
{
	__u64 head = __atomic_load_n(&r->meta->data_head, __ATOMIC_ACQUIRE);
	__u64 tail = r->meta->data_tail;
	int found = 0;

	while (tail < head && !found) {
		struct perf_event_header h;
		size_t off = tail % r->size;

		for (size_t i = 0; i < sizeof(h); i++)
			((char *)&h)[i] = r->data[(off + i) % r->size];
		if (h.type == PERF_RECORD_SAMPLE && h.size >= sizeof(struct sample)) {
			struct sample s;

			for (size_t i = 0; i < sizeof(s); i++)
				((char *)&s)[i] = r->data[(off + i) % r->size];
			if ((int)s.pid == pid) {
				*time = s.time;
				found = 1;
			}
		} else if (h.type == PERF_RECORD_LOST) {
			(*lost)++;
		}
		tail += h.size;
	}
	__atomic_store_n(&r->meta->data_tail, tail, __ATOMIC_RELEASE);
	return found;
}

int main(int argc, char **argv)
{
	static struct ring rings[MAX_CPUS];
	static struct pollfd pfds[MAX_CPUS];
	unsigned long long frames = 0, over = 0, budget_ns;
	unsigned long lines = 0, lost = 0;
	__u64 last_mark = 0;
	const char *lib;
	int pid, type, ncpus;
	FILE *out;

	if (argc != 6) {
		fprintf(stderr, "usage: %s PID LIB OFFSET BUDGET_US OUT\n", argv[0]);
		return 2;
	}
	pid = atoi(argv[1]);
	lib = argv[2];
	budget_ns = strtoull(argv[4], NULL, 0) * 1000;
	out = fopen(argv[5], "w");
	if (!out || nl_open()) {
		perror("delayacct_frames: setup");
		return 1;
	}
	type = uprobe_type();
	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (type < 0 || ncpus > MAX_CPUS) {
		fprintf(stderr, "delayacct_frames: no uprobe PMU\n");
		return 1;
	}
	for (int c = 0; c < ncpus; c++) {
		if (open_ring(&rings[c], type, lib, strtoull(argv[3], NULL, 0), c)) {
			perror("delayacct_frames: perf_event_open");
			return 1;
		}
		pfds[c].fd = rings[c].fd;
		pfds[c].events = POLLIN;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	refresh_threads(pid);
	fprintf(stderr, "Tracing delays of %d threads of PID %d\n", nthreads, pid);

	while (!stop) {
		char path[32];
		struct stat st;

		if (poll(pfds, ncpus, 200) < 0 && errno != EINTR)
			break;
		for (int c = 0; c < ncpus; c++) {
			__u64 t;

			while (next_sample(&rings[c], pid, &t, &lost)) {
				const unsigned long long d = last_mark ? t - last_mark : 0;
				const int late = d > budget_ns;

				frames++;
				over += late;
				if (frames % REFRESH_FRAMES == 0)
					refresh_threads(pid);
				if (late) {
					lines += read_frame(out, frames, t, d, 1);
					fflush(out);	/* so output can be measured while it runs */
				} else {
					read_frame(out, frames, t, d, 0);
				}
				last_mark = t;
			}
		}
		snprintf(path, sizeof(path), "/proc/%d", pid);
		if (stat(path, &st))
			break;
	}
	fclose(out);
	fprintf(stderr, "stats: frames=%llu over_budget=%llu lines=%lu lost=%lu\n", frames, over, lines, lost);
	return 0;
}
