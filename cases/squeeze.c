// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * squeeze: confine another process's threads to too few CPUs, in bursts.
 *
 * Stands in for a scheduler that places a game's threads badly for a while
 * (packing them onto one core while others sit idle), or for a launcher that
 * hands the game a wrong affinity mask. Every PERIOD it restricts every
 * thread of PID to the CPUs in -c for BURST, then gives the old masks back.
 * Nothing else competes with the game: its threads only get in each other's
 * way.
 *
 * Every burst is logged as [start, end] on CLOCK_MONOTONIC (the clock behind
 * bpf_ktime_get_ns()), from before the first mask changes to after the last
 * is restored.
 *
 * usage: squeeze -P PID [-c CPULIST] [-p PERIOD_MS] [-b BURST_MS]
 *                [-d SECONDS] [-o LOG]
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NSEC	1000000000ULL
#define MAX_TIDS 256

static int opt_pid, opt_period_ms = 1500, opt_burst_ms = 300, opt_duration = 20;
static const char *opt_cpus = "0";
static const char *opt_log;
static volatile sig_atomic_t stop;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NSEC + ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
	struct timespec ts = { .tv_sec = t / NSEC, .tv_nsec = t % NSEC };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		if (stop)
			return;
}

static void on_signal(int sig)
{
	stop = 1;
}

/* "0", "0-1", "0,2-3" */
static int parse_cpus(const char *s, cpu_set_t *set)
{
	CPU_ZERO(set);
	while (*s) {
		char *end;
		long a = strtol(s, &end, 10), b = a;

		if (end == s)
			return -1;
		if (*end == '-') {
			s = end + 1;
			b = strtol(s, &end, 10);
			if (end == s)
				return -1;
		}
		for (long c = a; c <= b && c < CPU_SETSIZE; c++)
			CPU_SET(c, set);
		s = *end == ',' ? end + 1 : end;
		if (*end && *end != ',')
			return -1;
	}
	return CPU_COUNT(set) ? 0 : -1;
}

struct saved {
	pid_t tid;
	cpu_set_t mask;
};

/* Narrow every thread that exists now; remember what each had. */
static int squeeze(const cpu_set_t *narrow, struct saved *sv)
{
	char path[64];
	struct dirent *de;
	int n = 0;
	DIR *d;

	snprintf(path, sizeof(path), "/proc/%d/task", opt_pid);
	d = opendir(path);
	if (!d)
		return -1;
	while ((de = readdir(d)) && n < MAX_TIDS) {
		pid_t tid = atoi(de->d_name);

		if (tid <= 0 || sched_getaffinity(tid, sizeof(sv[n].mask), &sv[n].mask))
			continue;
		if (sched_setaffinity(tid, sizeof(*narrow), narrow))
			continue;
		sv[n++].tid = tid;
	}
	closedir(d);
	return n;
}

int main(int argc, char **argv)
{
	struct saved sv[MAX_TIDS];
	cpu_set_t narrow;
	FILE *log = NULL;
	uint64_t t0, next;
	int c, nburst = 0;

	while ((c = getopt(argc, argv, "P:c:p:b:d:o:h")) != -1) {
		switch (c) {
		case 'P': opt_pid = atoi(optarg); break;
		case 'c': opt_cpus = optarg; break;
		case 'p': opt_period_ms = atoi(optarg); break;
		case 'b': opt_burst_ms = atoi(optarg); break;
		case 'd': opt_duration = atoi(optarg); break;
		case 'o': opt_log = optarg; break;
		default:
			fprintf(stderr, "usage: %s -P PID [-c CPULIST] [-p PERIOD_MS] "
				"[-b BURST_MS] [-d SECONDS] [-o LOG]\n", argv[0]);
			return c == 'h' ? 0 : 2;
		}
	}
	if (opt_pid <= 0 || parse_cpus(opt_cpus, &narrow) ||
	    opt_burst_ms <= 0 || opt_period_ms <= opt_burst_ms) {
		fprintf(stderr, "squeeze: need -P PID, a CPU list, and burst < period\n");
		return 2;
	}
	if (opt_log && !(log = fopen(opt_log, "w"))) {
		perror(opt_log);
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	fprintf(stderr, "squeeze pid=%d onto cpus %s, %d ms every %d ms\n",
		opt_pid, opt_cpus, opt_burst_ms, opt_period_ms);

	t0 = now_ns();
	next = t0 + (uint64_t)opt_period_ms * 1000000;
	while (!stop && next < t0 + (uint64_t)opt_duration * NSEC) {
		uint64_t start, end;
		int n;

		sleep_until(next);
		if (stop)
			break;
		start = now_ns();
		n = squeeze(&narrow, sv);
		if (n < 0)
			break;			/* the process is gone */
		sleep_until(start + (uint64_t)opt_burst_ms * 1000000);
		for (int i = 0; i < n; i++)
			sched_setaffinity(sv[i].tid, sizeof(sv[i].mask), &sv[i].mask);
		end = now_ns();
		if (log) {
			fprintf(log, "{\"burst\":%d,\"start_ns\":%llu,\"end_ns\":%llu,"
				"\"threads\":%d}\n", nburst, (unsigned long long)start,
				(unsigned long long)end, n);
			fflush(log);
		}
		nburst++;
		next += (uint64_t)opt_period_ms * 1000000;
	}
	if (log)
		fclose(log);
	fprintf(stderr, "squeeze: %d bursts\n", nburst);
	return 0;
}
