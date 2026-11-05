// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "analysis/analysis.h"

#include <algorithm>
#include <optional>

namespace ht {

namespace {
constexpr int MaxDepth = 5;
}

struct FrameCache {
	QHash<std::int32_t, QString> names;
	QHash<std::int32_t, std::vector<const Event *>> byTid;
	std::vector<std::int32_t> order;	// threads, as the record lists them
	std::vector<Event> open;		// waits still open at frame end
	std::optional<std::vector<Segment>> critical;
	std::optional<std::array<std::int64_t, CatCount>> totals;
};

QString catLabel(Cat c)
{
	switch (c) {
	case Cat::Run: return QStringLiteral("running");
	case Cat::Kern: return QStringLiteral("in the kernel (faults, reclaim, compaction)");
	case Cat::Sched: return QStringLiteral("waiting for a CPU");
	case Cat::Lock: return QStringLiteral("waiting on a lock");
	case Cat::Io: return QStringLiteral("waiting on the disk");
	case Cat::Sleep: return QStringLiteral("timer sleep");
	case Cat::Disp: return QStringLiteral("in present (display)");
	case Cat::Other: return QStringLiteral("other waits");
	case Cat::Open: return QStringLiteral("still waiting at frame end");
	default: return {};
	}
}

Cat catOf(Cause c)
{
	switch (c) {
	case Cause::Futex: return Cat::Lock;
	case Cause::Present: case Cause::Gpu: return Cat::Disp;
	case Cause::Timer: return Cat::Sleep;
	case Cause::Io: return Cat::Io;
	case Cause::Runnable: case Cause::Runqueue: return Cat::Sched;
	case Cause::Open: return Cat::Open;
	default: return Cat::Other;
	}
}

QString causeWord(Cause c)
{
	switch (c) {
	case Cause::Futex: return QStringLiteral("a lock");
	case Cause::Present: return QStringLiteral("the display");
	case Cause::Gpu: return QStringLiteral("the GPU");
	case Cause::Poll: return QStringLiteral("poll");
	case Cause::Task: return QStringLiteral("a reply");
	case Cause::Timer: return QStringLiteral("a timer");
	case Cause::Io: return QStringLiteral("the disk");
	case Cause::Open: return QStringLiteral("something");
	default: return QStringLiteral("something else");
	}
}

QString kernPartLabel(KernPart p)
{
	switch (p) {
	case KernPart::Fault: return QStringLiteral("page faults");
	case KernPart::FaultMajor: return QStringLiteral("page faults that read from disk");
	case KernPart::Reclaim: return QStringLiteral("reclaim");
	case KernPart::Compact: return QStringLiteral("compaction");
	default: return {};
	}
}

QStringList Segment::keys() const
{
	QStringList k;
	for (const auto &n : path)
		k << n.key;
	return k;
}

static FrameCache &cache(const Run &run, const Frame &f)
{
	if (f.cache)
		return *f.cache;
	auto c = std::make_shared<FrameCache>();
	QHash<QString, int> count;
	for (const auto &t : f.threads)
		count[t.comm]++;
	for (const auto &t : f.threads) {
		c->names.insert(t.tid, t.tid == f.rootTid || count.value(t.comm) == 1
			? t.comm : QStringLiteral("%1/%2").arg(t.comm).arg(t.tid));
		c->byTid.insert(t.tid, {});
		c->order.push_back(t.tid);
	}
	for (const auto &e : f.events) {
		if (!c->byTid.contains(e.tid))
			c->order.push_back(e.tid);
		c->byTid[e.tid].push_back(&e);
	}
	// a thread still off the CPU when the frame closed has no event yet
	c->open.reserve(f.threads.size());
	for (const auto &t : f.threads) {
		if (t.open <= 0)
			continue;
		Event e;
		e.tid = t.tid;
		e.out = f.duration - t.open;
		e.wake = e.in = f.duration;
		e.cause = Cause::Open;
		c->open.push_back(e);
	}
	for (const auto &e : c->open)
		c->byTid[e.tid].push_back(&e);
	for (auto &v : c->byTid)
		std::sort(v.begin(), v.end(), [](const Event *x, const Event *y) { return x->out < y->out; });
	(void)run;
	f.cache = c;
	return *c;
}

QString threadName(const Run &run, const Frame &f, std::int32_t tid)
{
	const auto &c = cache(run, f);
	auto it = c.names.find(tid);
	return it != c.names.end() ? *it : QStringLiteral("thread %1").arg(tid);
}

const std::vector<const Event *> &threadEvents(const Run &run, const Frame &f, std::int32_t tid)
{
	static const std::vector<const Event *> none;
	const auto &c = cache(run, f);
	auto it = c.byTid.find(tid);
	return it != c.byTid.end() ? *it : none;
}

std::vector<std::int32_t> frameThreads(const Run &run, const Frame &f)
{
	return cache(run, f).order;
}

/* ------------------------------------------------------------- the nodes */

static Node runNode(const QString &nm)
{
	return { nm + "|run", nm + " running", QStringLiteral("running"), {}, {}, Cat::Run };
}

static Node schedNode(const QString &nm, const Event &e, const Run &run)
{
	const QString who = (e.flags & FlagPreemptor) && !e.byComm.isEmpty() ? e.byComm : QString();
	Node n;
	n.key = nm + "|cpu|" + who;
	n.cat = Cat::Sched;
	n.label = who.isEmpty() ? nm + " waiting for a CPU" : nm + " waiting for a CPU, " + who + " had it";
	n.shortLabel = who.isEmpty() ? QStringLiteral("waiting for a CPU") : "CPU taken by " + who;
	if (!who.isEmpty() && e.byTgid != run.pid)
		n.ext = who;
	return n;
}

static Node wokenNode(const QString &nm)
{
	return { nm + "|woken", nm + " woken, waiting for a CPU", QStringLiteral("woken, no CPU yet"), {}, {},
		 Cat::Sched };
}

static Node waitNode(const QString &nm, const QString &wnm, const Event &e)
{
	Node n;
	n.key = nm + "|wait|" + wnm + "|" + QString::number(int(e.cause));
	n.cat = catOf(e.cause);
	n.label = nm + " waited for " + wnm;
	n.shortLabel = "waited for " + wnm;
	n.via = causeWord(e.cause);
	n.ev = &e;
	return n;
}

static Node termNode(const QString &nm, const Event &e, const Run &run)
{
	Node n;
	n.cat = catOf(e.cause);
	n.ev = e.cause == Cause::Open ? nullptr : &e;
	const bool irq = e.flags & FlagsInterrupt;
	if (e.cause == Cause::Open) {
		n.key = nm + "|open";
		n.label = nm + " still waiting at frame end";
		n.shortLabel = QStringLiteral("still waiting");
		return n;
	}
	if (!irq && e.byTgid && e.byTgid != run.pid && !e.byComm.isEmpty()) {
		n.key = nm + "|ext|" + e.byComm + "|" + QString::number(int(e.cause));
		n.label = nm + " waited for " + e.byComm + " (another process)";
		n.shortLabel = "waited for " + e.byComm;
		n.via = causeWord(e.cause);
		n.ext = e.byComm;
		return n;
	}
	QString what;
	switch (n.cat) {
	case Cat::Io: what = QStringLiteral("waiting on the disk"); break;
	case Cat::Sleep: what = QStringLiteral("sleeping on a timer"); break;
	case Cat::Disp: what = QStringLiteral("in present"); break;
	case Cat::Lock: what = QStringLiteral("waiting on a lock"); break;
	default: what = "waiting on " + causeWord(e.cause); break;
	}
	n.key = nm + "|" + QString::number(int(e.cause)) + (irq ? "|irq" : "|");
	n.label = nm + " " + what + (irq ? QStringLiteral(" (ended by an interrupt)") : QString());
	n.shortLabel = what;
	return n;
}

/* -------------------------------------------------------- the explanation */

static void explainInto(const Run &run, const Frame &f, std::int32_t tid, std::int64_t a, std::int64_t b,
			int depth, const std::vector<Node> &path, std::vector<std::int32_t> &seen,
			std::vector<Segment> &out)
{
	const auto &c = cache(run, f);
	const QString nm = threadName(run, f, tid);
	auto push = [&](std::int64_t x, std::int64_t y, Node node, const Event *ev) {
		if (y <= x)
			return;
		Segment s;
		s.a = x;
		s.b = y;
		s.tid = tid;
		s.cat = node.cat;
		s.path = path;
		s.path.push_back(std::move(node));
		s.ev = ev;
		out.push_back(std::move(s));
	};
	std::int64_t cur = a;
	for (const Event *e : threadEvents(run, f, tid)) {
		if (e->in <= cur || e->out >= b)
			continue;
		const std::int64_t s = std::max(e->out, cur);
		if (s > cur)
			push(cur, s, runNode(nm), nullptr);
		if (e->cause == Cause::Runnable) {
			const std::int64_t y = std::min(e->in, b);
			push(s, y, schedNode(nm, *e, run), e);
			cur = std::max(cur, y);
			continue;
		}
		const std::int64_t end = std::min(e->wake, b);
		if (end > s) {
			const bool byGame = e->byTgid == run.pid && e->byTid && e->byTid != tid
				&& !(e->flags & FlagsInterrupt)
				&& std::find(seen.begin(), seen.end(), e->byTid) == seen.end()
				&& depth < MaxDepth && c.byTid.contains(e->byTid);
			if (byGame) {
				std::vector<Node> p2 = path;
				p2.push_back(waitNode(nm, threadName(run, f, e->byTid), *e));
				seen.push_back(tid);
				explainInto(run, f, e->byTid, s, end, depth + 1, p2, seen, out);
				seen.pop_back();
			} else {
				push(s, end, termNode(nm, *e, run), e);
			}
		}
		const std::int64_t q0 = std::max(e->wake, s), q1 = std::min(e->in, b);
		if (q1 > q0)
			push(q0, q1, wokenNode(nm), e);
		cur = std::max(cur, std::min(e->in, b));
		if (cur >= b)
			break;
	}
	if (cur < b)
		push(cur, b, runNode(nm), nullptr);
}

std::vector<Segment> explain(const Run &run, const Frame &f, std::int32_t tid, std::int64_t a, std::int64_t b)
{
	std::vector<Segment> out;
	std::vector<std::int32_t> seen;
	explainInto(run, f, tid, a, b, 0, {}, seen, out);
	return out;
}

const std::vector<Segment> &criticalPath(const Run &run, const Frame &f)
{
	auto &c = cache(run, f);
	if (!c.critical)
		c.critical = explain(run, f, f.rootTid, 0, f.duration);
	return *c.critical;
}

std::vector<Segment> explainWait(const Run &run, const Frame &f, const Event &e)
{
	return explain(run, f, e.tid, std::max<std::int64_t>(0, e.out), std::min(f.duration, e.wake));
}

static const StallGroup *groupFor(const Frame &f, const Event &e)
{
	if (e.tid != f.rootTid)
		return nullptr;
	const bool irq = e.flags & FlagsInterrupt;
	for (const auto &g : f.stalls)
		if (!g.other && g.cause == e.cause && (irq ? g.byTid == 0 || g.blockedIn == e.blockedIn : g.byTid == e.byTid))
			return &g;
	return nullptr;
}

StackOf stackOf(const Run &run, const Frame &f, const Event &e)
{
	(void)run;
	StackOf s;
	const StallGroup *g = groupFor(f, e);
	if (g)
		s.user = g->ustack;
	if (auto it = f.kstacks.find(e.kstackId); e.kstackId >= 0 && it != f.kstacks.end() && !it->isEmpty()) {
		s.kernel = *it;
		s.source = StackOf::Exact;
		return s;
	}
	if (g && !g->kstack.isEmpty()) {
		s.kernel = g->kstack;
		s.source = StackOf::Group;
		return s;
	}
	for (const auto &t : f.threads)
		if (t.tid == e.tid && t.largestNs && t.largestIn == e.blockedIn && !t.largestKstack.isEmpty()) {
			s.kernel = t.largestKstack;
			s.source = StackOf::Largest;
			return s;
		}
	if (!e.blockedIn.isEmpty())
		s.kernel = { e.blockedIn };
	return s;
}

std::vector<Segment> merged(const std::vector<Segment> &segs)
{
	std::vector<Segment> out;
	for (const auto &s : segs) {
		if (!out.empty()) {
			Segment &p = out.back();
			if (p.b == s.a && p.tid == s.tid && p.path.size() == s.path.size()
			    && std::equal(p.path.begin(), p.path.end(), s.path.begin(),
					  [](const Node &x, const Node &y) { return x.key == y.key; })) {
				p.b = s.b;
				continue;
			}
		}
		out.push_back(s);
	}
	return out;
}

RootKernel rootKernel(const Run &run, const Frame &f)
{
	RootKernel k;
	for (const auto &s : criticalPath(run, f))
		if (s.path.size() == 1 && s.cat == Cat::Run)
			k.running += s.b - s.a;
	std::int64_t left = k.running;
	for (int i = 0; i < int(KernPart::Count); i++) {
		k.parts[i] = std::min(f.kern[i], left);
		left -= k.parts[i];
	}
	k.kernel = k.running - left;
	return k;
}

const std::array<std::int64_t, CatCount> &frameTotals(const Run &run, const Frame &f)
{
	auto &c = cache(run, f);
	if (!c.totals) {
		std::array<std::int64_t, CatCount> t{};
		for (const auto &s : criticalPath(run, f))
			t[int(s.cat)] += s.b - s.a;
		const RootKernel k = rootKernel(run, f);
		t[int(Cat::Run)] -= k.kernel;
		t[int(Cat::Kern)] += k.kernel;
		c.totals = t;
	}
	return *c.totals;
}

/* ------------------------------------------------------------ blame trees */

BlameNode *BlameNode::child(const Node &n)
{
	if (auto it = byKey.find(n.key); it != byKey.end())
		return *it;
	auto k = std::make_unique<BlameNode>();
	k->key = n.key;
	k->label = n.label;
	k->shortLabel = n.shortLabel;
	k->via = n.via;
	k->ext = n.ext;
	k->cat = n.cat;
	BlameNode *p = k.get();
	byKey.insert(n.key, p);
	kids.push_back(std::move(k));
	return p;
}

BlameNode *BlameNode::find(const QStringList &keys)
{
	BlameNode *n = this;
	for (const auto &k : keys) {
		auto it = n->byKey.find(k);
		if (it == n->byKey.end())
			return nullptr;
		n = *it;
	}
	return n;
}

std::vector<const BlameNode *> BlameNode::sortedKids() const
{
	std::vector<const BlameNode *> v;
	for (const auto &k : kids)
		v.push_back(k.get());
	std::sort(v.begin(), v.end(), [](const BlameNode *x, const BlameNode *y) { return x->ns > y->ns; });
	return v;
}

static void addSegs(BlameNode &tree, const Run &run, const Frame &f, const std::vector<Segment> &segs,
		    bool withKernel)
{
	for (const auto &s : segs) {
		const std::int64_t d = s.b - s.a;
		tree.ns += d;
		BlameNode *n = &tree;
		for (const auto &p : s.path) {
			n = n->child(p);
			n->ns += d;
			n->frames.insert(f.id);
		}
	}
	if (!withKernel)
		return;
	// the presenting thread's running time, split into the game's code and the kernel
	const RootKernel k = rootKernel(run, f);
	if (!k.kernel)
		return;
	const QString nm = threadName(run, f, f.rootTid);
	auto it = tree.byKey.find(nm + "|run");
	if (it == tree.byKey.end())
		return;
	BlameNode *rn = *it;
	auto add = [&](const QString &key, const QString &label, Cat cat, std::int64_t ns) {
		if (ns <= 0)
			return;
		BlameNode *c = rn->child({ key, label, label, {}, {}, cat });
		c->ns += ns;
		c->frames.insert(f.id);
	};
	for (int i = 0; i < int(KernPart::Count); i++)
		add(QStringLiteral("kern%1").arg(i), "in the kernel: " + kernPartLabel(KernPart(i)), Cat::Kern,
		    k.parts[i]);
	add(QStringLiteral("code"), QStringLiteral("the game's own code"), Cat::Run, k.running - k.kernel);
}

std::unique_ptr<BlameNode> blameFrames(const Run &run, const std::vector<const Frame *> &frames,
				       const QString &label)
{
	auto tree = std::make_unique<BlameNode>();
	tree->key = QStringLiteral("all");
	tree->label = label;
	for (const Frame *f : frames)
		addSegs(*tree, run, *f, criticalPath(run, *f), true);
	tree->frameCount = int(frames.size());
	return tree;
}

std::unique_ptr<BlameNode> blameWait(const Run &run, const Frame &f, const Event &e, const QString &label)
{
	auto tree = std::make_unique<BlameNode>();
	tree->key = QStringLiteral("all");
	tree->label = label;
	addSegs(*tree, run, f, explainWait(run, f, e), false);
	tree->frameCount = 1;
	return tree;
}

}  // namespace ht
