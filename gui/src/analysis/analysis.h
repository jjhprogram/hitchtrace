// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// What the views show, derived from one frame's record:
//
//   explain()       what decided how a stretch of time went on one thread: its
//                   own running and waiting and, for a wait another of the
//                   game's threads ended, what that thread did meanwhile,
//                   recursively. The result tiles the stretch exactly.
//   criticalPath()  explain() over the presenting thread's whole frame.
//   blame trees     segments summed by chain: each level is one hop.
#pragma once

#include "data/record.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <array>
#include <memory>
#include <vector>

namespace ht {

// What time was spent on, in drawing order. Also the colour of a mark.
enum class Cat : std::uint8_t { Run, Kern, Sched, Lock, Io, Sleep, Disp, Other, Open, Count };
constexpr int CatCount = int(Cat::Count);
QString catLabel(Cat c);
Cat catOf(Cause c);
QString causeWord(Cause c);	// "a lock", "the disk", ...
QString kernPartLabel(KernPart p);

// One hop of a chain. `key` is the same across frames; `label` is shown.
struct Node {
	QString key, label, shortLabel;
	QString via;	// what the wait was on, for a hop to another thread
	QString ext;	// another process at this hop, not traced
	Cat cat = Cat::Run;
	const Event *ev = nullptr;	// the wait this hop was in, if it was a wait
};

struct Segment {
	std::int64_t a = 0, b = 0;
	std::int32_t tid = 0;
	Cat cat = Cat::Run;
	std::vector<Node> path;		// from the thread the explanation started on
	const Event *ev = nullptr;	// the interval it came from, if any

	const Node &last() const { return path.back(); }
	QStringList keys() const;
};

QString threadName(const Run &run, const Frame &f, std::int32_t tid);
const std::vector<const Event *> &threadEvents(const Run &run, const Frame &f, std::int32_t tid);
std::vector<std::int32_t> frameThreads(const Run &run, const Frame &f);

std::vector<Segment> explain(const Run &run, const Frame &f, std::int32_t tid,
			     std::int64_t a, std::int64_t b);
const std::vector<Segment> &criticalPath(const Run &run, const Frame &f);
// Adjacent segments with the same chain, joined (for drawing).
std::vector<Segment> merged(const std::vector<Segment> &segs);

struct RootKernel {
	std::int64_t running = 0;	// the presenting thread's own running segments
	std::int64_t kernel = 0;	// of which in the kernel (amount, not position)
	std::array<std::int64_t, int(KernPart::Count)> parts{};
};
RootKernel rootKernel(const Run &run, const Frame &f);
// The critical path's time by category, with the kernel's share split out.
const std::array<std::int64_t, CatCount> &frameTotals(const Run &run, const Frame &f);

struct BlameNode {
	QString key, label, shortLabel, via, ext;
	Cat cat = Cat::Run;
	std::int64_t ns = 0;
	QSet<std::uint64_t> frames;	// which frames this node appears in
	int frameCount = 0;		// on the root: how many frames were summed
	std::vector<std::unique_ptr<BlameNode>> kids;
	QHash<QString, BlameNode *> byKey;

	BlameNode *child(const Node &n);
	BlameNode *find(const QStringList &keys);
	std::vector<const BlameNode *> sortedKids() const;
};

std::unique_ptr<BlameNode> blameFrames(const Run &run, const std::vector<const Frame *> &frames,
				       const QString &label);
std::unique_ptr<BlameNode> blameWait(const Run &run, const Frame &f, const Event &e,
				     const QString &label);
// A wait's own chain, as segments over [out, wake).
std::vector<Segment> explainWait(const Run &run, const Frame &f, const Event &e);

// Where a wait was blocked, as precisely as the recording allows.
struct StackOf {
	QStringList kernel;	// innermost frame first
	QStringList user;	// the game's own frames, innermost first; may be empty
	enum Source {
		Exact,		// this wait's own stack
		Group,		// the stack of the longest wait in its stall group
		Largest,	// the stack of its thread's longest wait, which blocked in the same place
		TopOnly,	// only the function it blocked in is known
	} source = TopOnly;
};
StackOf stackOf(const Run &run, const Frame &f, const Event &e);

}  // namespace ht
