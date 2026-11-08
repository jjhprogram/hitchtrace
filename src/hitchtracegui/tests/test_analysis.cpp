// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// Checks the analysis on the case-study recordings in cases/out:
//  - every frame's critical path tiles the frame exactly, and its category
//    totals add up to the frame;
//  - for one typical frame per case, the totals match the figures the HTML
//    mockup computed from the same records (tools' JS, to 0.15 ms).
// A case whose recording is missing is skipped, not failed.
#include "analysis/analysis.h"

#include <QFileInfo>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace ht;

static int failures;

#define CHECK(cond, ...)                                    \
	do {                                                \
		if (!(cond)) {                              \
			std::fprintf(stderr, "FAIL: " __VA_ARGS__); \
			std::fputc('\n', stderr);           \
			failures++;                         \
		}                                           \
	} while (0)

struct Expect {
	const char *run;
	std::uint64_t frame;
	std::map<Cat, double> ms;	// categories not listed must be under 0.15 ms
	const char *label;		// a chain label the critical path must contain
};

int main()
{
	const std::vector<Expect> expect = {
		{ "bgcpu-timeline", 2241, { { Cat::Run, 8.8 }, { Cat::Sched, 8.2 }, { Cat::Sleep, 6.7 } },
		  "mg_worker0 waiting for a CPU, bgload2 had it" },
		{ "iothread-timeline", 2129, { { Cat::Run, 8.5 }, { Cat::Sched, 0.4 }, { Cat::Io, 17.1 }, { Cat::Sleep, 6.5 } },
		  "mg_stream waiting on the disk (ended by an interrupt)" },
		{ "iodisk", 2144, { { Cat::Run, 19.9 }, { Cat::Sched, 0.5 }, { Cat::Io, 63.0 }, { Cat::Sleep, 2.8 } },
		  "mg_main waited for mg_stream" },
		{ "mem", 779, { { Cat::Run, 16.0 }, { Cat::Kern, 3.6 }, { Cat::Sched, 0.7 }, { Cat::Io, 16.6 }, { Cat::Sleep, 2.3 } },
		  "mg_main waiting on the disk (ended by an interrupt)" },
		{ "slack", 1030, { { Cat::Run, 14.7 }, { Cat::Sched, 0.7 }, { Cat::Sleep, 11.1 } },
		  "mg_main sleeping on a timer (ended by an interrupt)" },
		{ "ipc", 2475, { { Cat::Run, 15.1 }, { Cat::Sched, 1.6 }, { Cat::Other, 7.9 } },
		  "mg_main waited for mg_server (another process)" },
		{ "cpuspike", 2009, { { Cat::Run, 26.8 }, { Cat::Sched, 0.3 }, { Cat::Sleep, 1.7 } }, "mg_main running" },
		{ "squeeze", 540, { { Cat::Run, 15.6 }, { Cat::Sched, 31.1 } },
		  "mg_worker0 waiting for a CPU, mg_worker1 had it" },
		{ "alloc", 1604, { { Cat::Run, 15.6 }, { Cat::Kern, 12.0 }, { Cat::Sched, 0.8 }, { Cat::Sleep, 1.1 } },
		  "mg_main running" },
		{ "compact", 1514, { { Cat::Run, 15.5 }, { Cat::Kern, 32.2 }, { Cat::Sched, 0.2 } }, "mg_main running" },
	};

	int runs = 0, frames = 0;
	for (const auto &x : expect) {
		const QString path = QStringLiteral(CASES_DIR "/%1/hitch.jsonl").arg(QLatin1String(x.run));
		if (!QFileInfo::exists(path)) {
			std::printf("skip %s: no recording\n", x.run);
			continue;
		}
		QString err;
		auto run = loadRun(path, &err);
		CHECK(run, "%s", qPrintable(err));
		if (!run)
			continue;
		runs++;
		int bad = 0;
		for (const auto &f : run->frames) {
			frames++;
			std::int64_t cur = 0;
			bool tiles = true;
			for (const auto &s : criticalPath(*run, f)) {
				tiles &= s.a == cur && s.b > s.a;
				cur = s.b;
			}
			tiles &= cur == f.duration;
			std::int64_t sum = 0;
			for (auto v : frameTotals(*run, f))
				sum += v;
			if (!tiles || sum != f.duration)
				bad++;
		}
		CHECK(!bad, "%s: %d frames whose critical path does not tile the frame", x.run, bad);

		const Frame *f = nullptr;
		for (const auto &fr : run->frames)
			if (fr.id == x.frame)
				f = &fr;
		CHECK(f, "%s: no frame %llu", x.run, (unsigned long long)x.frame);
		if (!f)
			continue;
		const auto &t = frameTotals(*run, *f);
		for (int c = 0; c < CatCount; c++) {
			auto it = x.ms.find(Cat(c));
			const double want = it == x.ms.end() ? 0.0 : it->second;
			const double got = t[c] / 1e6;
			CHECK(std::abs(got - want) <= 0.15, "%s frame %llu: %s %.2f ms, expected %.1f", x.run,
			      (unsigned long long)x.frame, qPrintable(catLabel(Cat(c))), got, want);
		}
		bool found = false;
		for (const auto &s : criticalPath(*run, *f))
			for (const auto &n : s.path)
				found |= n.label == QLatin1String(x.label);
		CHECK(found, "%s frame %llu: no \"%s\" on the critical path", x.run, (unsigned long long)x.frame,
		      x.label);
		std::printf("ok   %-18s %4zu frames, frame %llu as expected\n", x.run, run->frames.size(),
			    (unsigned long long)x.frame);
	}
	std::printf("%d runs, %d frames, %d failures\n", runs, frames, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
