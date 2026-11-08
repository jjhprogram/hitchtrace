// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "data/record.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace ht {

Cause causeFromString(const QString &s)
{
	if (s == QLatin1String("block_futex")) return Cause::Futex;
	if (s == QLatin1String("block_present")) return Cause::Present;
	if (s == QLatin1String("block_poll")) return Cause::Poll;
	if (s == QLatin1String("block_timer")) return Cause::Timer;
	if (s == QLatin1String("block_io")) return Cause::Io;
	if (s == QLatin1String("runnable")) return Cause::Runnable;
	if (s == QLatin1String("block_task")) return Cause::Task;
	if (s == QLatin1String("block_gpu")) return Cause::Gpu;
	if (s == QLatin1String("runqueue")) return Cause::Runqueue;
	return Cause::Other;
}

static std::uint32_t flagsFrom(const QJsonArray &a)
{
	static const char *names[] = { "irq", "softirq", "irqexit", "idle", "hist", "preemptor", "stale" };
	std::uint32_t f = 0;
	for (const auto &v : a)
		for (unsigned i = 0; i < std::size(names); i++)
			if (v.toString() == QLatin1String(names[i]))
				f |= 1u << i;
	return f;
}

static qint64 ns(const QJsonValue &v)
{
	return v.toInteger();
}

static QStringList strings(const QJsonArray &a)
{
	QStringList out;
	for (const auto &v : a)
		out << v.toString();
	return out;
}

std::unique_ptr<Run> loadRun(const QString &path, QString *error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		*error = QStringLiteral("%1: %2").arg(path, file.errorString());
		return nullptr;
	}
	auto run = std::make_unique<Run>();
	run->path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
	run->title = QFileInfo(path).dir().dirName();
	qint64 traceStart = -1;
	int lineNo = 0;
	while (!file.atEnd()) {
		const QByteArray line = file.readLine().trimmed();
		lineNo++;
		if (line.isEmpty())
			continue;
		QJsonParseError perr;
		const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
		if (!doc.isObject()) {
			*error = QStringLiteral("%1:%2: not JSON: %3").arg(path).arg(lineNo).arg(perr.errorString());
			return nullptr;
		}
		const QJsonObject o = doc.object();
		if (o.value(QLatin1String("type")).toString() == QLatin1String("header")) {
			traceStart = ns(o.value(QLatin1String("trace_start_ns")));
			run->pid = o.value(QLatin1String("pid")).toInt();
			run->budget = ns(o.value(QLatin1String("budget_ns")));
			continue;
		}
		if (!o.contains(QLatin1String("frame_id")))
			continue;

		Frame f;
		const qint64 fs = ns(o.value(QLatin1String("frame_start_ns")));
		f.id = o.value(QLatin1String("frame_id")).toInteger();
		f.start = fs;	// made trace-relative below, once the start is known
		f.duration = ns(o.value(QLatin1String("frame_ns")));
		f.budget = ns(o.value(QLatin1String("budget_ns")));
		f.rootTid = o.value(QLatin1String("root_pid")).toInt();
		const QJsonObject c = o.value(QLatin1String("cause_ns")).toObject();
		f.kern[int(KernPart::Fault)] = ns(c.value(QLatin1String("oncpu_fault")));
		f.kern[int(KernPart::FaultMajor)] = ns(c.value(QLatin1String("oncpu_fault_major")));
		f.kern[int(KernPart::Reclaim)] = ns(c.value(QLatin1String("oncpu_reclaim")));
		f.kern[int(KernPart::Compact)] = ns(c.value(QLatin1String("oncpu_compact")));
		f.oncpu = ns(c.value(QLatin1String("oncpu")));
		for (const auto &tv : o.value(QLatin1String("threads")).toArray()) {
			const QJsonObject t = tv.toObject();
			Thread th;
			th.tid = t.value(QLatin1String("tid")).toInt();
			th.comm = t.value(QLatin1String("comm")).toString();
			th.root = t.value(QLatin1String("root")).toBool();
			th.open = ns(t.value(QLatin1String("open_ns")));
			const QJsonObject lg = t.value(QLatin1String("largest")).toObject();
			th.largestNs = ns(lg.value(QLatin1String("ns")));
			th.largestIn = lg.value(QLatin1String("blocked_in")).toString();
			th.largestKstack = strings(lg.value(QLatin1String("kstack")).toArray());
			f.threads.push_back(th);
		}
		for (const auto &sv : o.value(QLatin1String("stalls")).toArray()) {
			const QJsonObject s = sv.toObject();
			StallGroup g;
			g.cause = causeFromString(s.value(QLatin1String("cause")).toString());
			g.blockedIn = s.value(QLatin1String("blocked_in")).toString();
			g.kstack = strings(s.value(QLatin1String("kstack")).toArray());
			g.ustack = strings(s.value(QLatin1String("ustack")).toArray());
			g.ns = ns(s.value(QLatin1String("ns")));
			g.other = s.value(QLatin1String("other")).toBool();
			const QJsonArray hops = s.value(QLatin1String("hops")).toArray();
			if (!hops.isEmpty())
				g.byTid = hops[0].toObject().value(QLatin1String("pid")).toInt();
			f.stalls.push_back(std::move(g));
		}
		const QJsonObject table = o.value(QLatin1String("kstacks")).toObject();
		for (auto it = table.begin(); it != table.end(); ++it)
			f.kstacks.insert(it.key().toInt(), strings(it.value().toArray()));
		const QJsonValue evs = o.value(QLatin1String("events"));
		f.hasTimeline = evs.isArray();
		for (const auto &ev : evs.toArray()) {
			const QJsonObject e = ev.toObject();
			Event x;
			x.tid = e.value(QLatin1String("tid")).toInt();
			x.out = ns(e.value(QLatin1String("out_ns"))) - fs;
			x.wake = ns(e.value(QLatin1String("wake_ns"))) - fs;
			x.in = ns(e.value(QLatin1String("in_ns"))) - fs;
			x.cause = causeFromString(e.value(QLatin1String("cause")).toString());
			x.flags = flagsFrom(e.value(QLatin1String("flags")).toArray());
			x.blockedIn = e.value(QLatin1String("blocked_in")).toString();
			x.kstackId = e.contains(QLatin1String("kstack_id")) ? e.value(QLatin1String("kstack_id")).toInt() : -1;
			const QJsonObject by = e.value(QLatin1String("by")).toObject();
			x.byTid = by.value(QLatin1String("tid")).toInt();
			x.byTgid = by.value(QLatin1String("tgid")).toInt();
			x.byComm = by.value(QLatin1String("comm")).toString();
			x.byStall = ns(by.value(QLatin1String("stall_ns")));
			f.events.push_back(std::move(x));
		}
		run->frames.push_back(std::move(f));
	}
	if (run->frames.empty()) {
		*error = QStringLiteral("%1: no hitch records").arg(path);
		return nullptr;
	}
	std::sort(run->frames.begin(), run->frames.end(),
		  [](const Frame &a, const Frame &b) { return a.start < b.start; });
	if (traceStart < 0)
		traceStart = run->frames.front().start;
	if (!run->budget)
		run->budget = run->frames.front().budget;
	for (auto &f : run->frames) {
		f.start -= traceStart;
		run->span = std::max(run->span, f.start + f.duration);
	}
	return run;
}

}  // namespace ht
