// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// A hitchtrace recording as the viewer holds it: one Run per hitch.jsonl, one
// Frame per over-budget frame, and the frame's timeline of off-CPU intervals.
// Times are nanoseconds; frame-relative unless the name says otherwise.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ht {

// What an off-CPU interval waited for (hitchtrace's cause, for the timeline).
enum class Cause : std::uint8_t {
	Futex, Present, Poll, Timer, Io, Runnable, Task, Gpu, Other, Runqueue,
	Open,	// still off the CPU when the frame closed; no event recorded yet
};
Cause causeFromString(const QString &s);

// How a wait ended (HT_HOP_* flags as hitchtrace writes them).
enum EventFlag : std::uint32_t {
	FlagIrq = 1 << 0, FlagSoftirq = 1 << 1, FlagIrqExit = 1 << 2, FlagIdle = 1 << 3,
	FlagHist = 1 << 4, FlagPreemptor = 1 << 5, FlagStale = 1 << 6,
};
constexpr std::uint32_t FlagsInterrupt = FlagIrq | FlagSoftirq | FlagIrqExit | FlagIdle;

// One off-CPU interval of one of the game's threads.
struct Event {
	std::int32_t tid = 0;
	std::int64_t out = 0;	// left the CPU
	std::int64_t wake = 0;	// woken (== in for a preempted thread)
	std::int64_t in = 0;	// running again
	Cause cause = Cause::Other;
	std::uint32_t flags = 0;
	std::int32_t byTid = 0;		// who woke it, or who preempted it
	std::int32_t byTgid = 0;
	QString byComm;
	std::int64_t byStall = 0;	// of the wait, the waker was itself off the CPU
	QString blockedIn;		// the top frame of the blocking kernel stack
	std::int32_t kstackId = -1;	// its full stack in Frame::kstacks (newer recordings)
};

struct Thread {
	std::int32_t tid = 0;
	QString comm;
	bool root = false;
	std::int64_t open = 0;	// still off the CPU this long when the frame ended
	std::int64_t largestNs = 0;	// its longest wait in the frame ...
	QString largestIn;		// ... where it blocked ...
	QStringList largestKstack;	// ... and the kernel stack of that wait
};

// One of the presenting thread's stall groups: its waits with the same cause,
// ended by the same thread. The stacks are those of the group's longest wait.
struct StallGroup {
	Cause cause = Cause::Other;
	std::int32_t byTid = 0;		// the thread at the first hop, 0 for an interrupt
	QString blockedIn;
	QStringList kstack;
	QStringList ustack;		// the game's own frames; only with hitchtrace -U
	std::int64_t ns = 0;
	bool other = false;		// the catch-all for groups that did not fit
};

// The presenting thread's in-kernel running time, by kind.
enum class KernPart : int { Fault, FaultMajor, Reclaim, Compact, Count };

struct FrameCache;

struct Frame {
	std::uint64_t id = 0;
	std::int64_t start = 0;		// from the start of the trace
	std::int64_t duration = 0;
	std::int64_t budget = 0;
	std::int32_t rootTid = 0;
	std::array<std::int64_t, int(KernPart::Count)> kern{};
	std::int64_t oncpu = 0;		// the presenting thread's plain running time
	std::vector<Thread> threads;
	std::vector<Event> events;
	std::vector<StallGroup> stalls;
	QHash<int, QStringList> kstacks;	// the events' and hops' stacks, by id
	bool hasTimeline = false;

	// what the analysis derives from the frame, built on first use
	mutable std::shared_ptr<FrameCache> cache;
};

struct Run {
	QString path;
	QString title;		// the directory the file is in, e.g. "iodisk"
	std::int32_t pid = 0;
	std::int64_t budget = 0;
	std::int64_t span = 0;	// from the start of the trace to the last frame's end
	std::vector<Frame> frames;	// by start time; never resized after loading
};

// Reads a hitchtrace -o file. On failure returns nullptr and sets @error.
std::unique_ptr<Run> loadRun(const QString &path, QString *error);

}  // namespace ht
