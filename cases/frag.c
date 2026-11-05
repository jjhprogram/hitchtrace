// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * frag: leave the machine's free memory in small pieces.
 *
 * Stands in for a long-running desktop session: memory that has been
 * allocated and freed for hours has plenty free, but few free stretches of
 * 2 MB. frag gets there in a second. It takes all free memory but LEAVE_MB,
 * one small page at a time, then gives every other page back and keeps the
 * rest. What is free afterwards is single pages with a used page on either
 * side; a huge page can only be had by moving frag's pages out of the way
 * (compaction).
 *
 * It prints "ready" on stdout when the memory is in that state, and holds it
 * until killed.
 *
 * usage: frag [-l LEAVE_MB] [-m MAX_MB]
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static long meminfo_kb(const char *key)
{
	char line[256];
	long kb = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key))) {
			kb = atol(line + strlen(key));
			break;
		}
	fclose(f);
	return kb;
}

static void on_signal(int sig)
{
}

int main(int argc, char **argv)
{
	long leave_mb = 256, max_mb = 1L << 30, free_mb, mb;
	size_t bytes, i;
	unsigned char *p;
	int c;

	while ((c = getopt(argc, argv, "l:m:h")) != -1) {
		switch (c) {
		case 'l': leave_mb = atol(optarg); break;
		case 'm': max_mb = atol(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-l LEAVE_MB] [-m MAX_MB]\n", argv[0]);
			return c == 'h' ? 0 : 2;
		}
	}
	free_mb = meminfo_kb("MemFree:") / 1024;
	mb = free_mb - leave_mb;
	if (mb > max_mb)
		mb = max_mb;
	if (mb < 64) {
		fprintf(stderr, "frag: only %ld MB free, nothing to do\n", free_mb);
		return 1;
	}
	bytes = (size_t)mb << 20;
	p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	madvise(p, bytes, MADV_NOHUGEPAGE);
	for (i = 0; i < bytes; i += 4096)
		p[i] = 1;
	for (i = 4096; i < bytes; i += 8192)
		madvise(p + i, 4096, MADV_DONTNEED);
	fprintf(stderr, "frag: took %ld of %ld MB free, gave every other page back\n",
		mb, free_mb);
	puts("ready");
	fflush(stdout);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	pause();
	return 0;
}
