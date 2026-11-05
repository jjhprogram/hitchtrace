// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "lab_cases.h"

#include <QFile>
#include <algorithm>
#include <unistd.h>

namespace ht {

namespace {

Knob knob(const char *key, const QString &label, int value, int min, int max, const QString &suffix = {},
	  const QString &help = {})
{
	Knob k;
	k.key = QLatin1String(key);
	k.label = label;
	k.value = value;
	k.min = min;
	k.max = max;
	k.suffix = suffix;
	k.help = help;
	return k;
}

Knob toggle(const char *key, const QString &label, bool on, const QString &help = {})
{
	Knob k = knob(key, label, on, 0, 1, {}, help);
	k.toggle = true;
	return k;
}

QString num(int v)
{
	return QString::number(v);
}

QString cpuText(const QList<int> &cpus)
{
	QStringList parts;
	for (int i = 0; i < cpus.size();) {
		int j = i;
		while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1)
			j++;
		parts << (i == j ? num(cpus[i]) : QStringLiteral("%1-%2").arg(cpus[i]).arg(cpus[j]));
		i = j + 1;
	}
	return parts.join(QLatin1Char(','));
}

// whether systemd can limit the user's disk reads (the io controller delegated)
bool ioDelegated()
{
	const uid_t uid = ::getuid();
	QFile f(QStringLiteral("/sys/fs/cgroup/user.slice/user-%1.slice/user@%1.service/cgroup.controllers").arg(uid));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	return QString::fromLatin1(f.readAll()).simplified().split(QLatin1Char(' ')).contains(QStringLiteral("io"));
}

}  // namespace

QList<int> parseCpus(const QString &list)
{
	QList<int> out;
	for (const QString &part : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		const QStringList ab = part.trimmed().split(QLatin1Char('-'));
		bool ok1 = false, ok2 = true;
		const int a = ab[0].toInt(&ok1);
		const int b = ab.size() > 1 ? ab[1].toInt(&ok2) : a;
		if (!ok1 || !ok2 || ab.size() > 2 || a < 0 || b < a || b > 1023)
			return {};
		for (int c = a; c <= b; c++)
			if (!out.contains(c))
				out << c;
	}
	std::sort(out.begin(), out.end());
	return out;
}

QString packPath(const LabEnv &env)
{
	return env.dataDir + QStringLiteral("/pack.bin");
}

QString scratchPath(const LabEnv &env)
{
	return env.dataDir + QStringLiteral("/scratch.bin");
}

QList<LabCase> labCases()
{
	QList<LabCase> cs;
	auto add = [&](const char *key, const QString &title, const QString &what, const QString &look,
		       QList<Knob> knobs = {}, const QString &warning = {}) {
		cs.push_back({ QLatin1String(key), title, what, look, warning, knobs });
	};
	const QString ms = QStringLiteral(" ms"), mb = QStringLiteral(" MB");

	add("baseline", QStringLiteral("Nothing in the way"),
	    QStringLiteral("The game alone. Every frame does the same work; a few still run long, because the "
			   "machine is shared with the desktop."),
	    QStringLiteral("Few, short hitches. The critical path is the main thread waiting for its workers while "
			   "they ran: what a normal frame looks like, to compare the other cases with."));
	add("bgcpu", QStringLiteral("Another program takes the game's cores"),
	    QStringLiteral("A background program wakes now and then and burns CPU on several threads, on the same "
			   "cores as the game, like a launcher, an indexer or a browser tab."),
	    QStringLiteral("The main thread waits for a worker that was waiting for a CPU, and the record names who "
			   "had it: bgload."),
	    { knob("threads", QStringLiteral("Its threads"), 4, 1, 16),
	      knob("period", QStringLiteral("Wakes every"), 700, 50, 10000, ms),
	      knob("burst", QStringLiteral("Burns for"), 20, 1, 1000, ms) });
	add("iomain", QStringLiteral("The main thread reads from disk"),
	    QStringLiteral("Every so often the main thread itself reads an asset from disk before it can draw, "
			   "as at an area transition."),
	    QStringLiteral("Disk waits on the main thread's own lane, ended by the disk's interrupt."),
	    { knob("asset", QStringLiteral("Asset size"), 8, 1, 128, mb),
	      knob("every", QStringLiteral("Every"), 30, 2, 1000, QStringLiteral(" frames")) });
	add("iothread", QStringLiteral("The main thread waits for a streaming thread"),
	    QStringLiteral("The main thread asks its streaming thread for an asset and waits for it: asynchronous "
			   "loading in name only."),
	    QStringLiteral("The main thread waited for mg_stream, which was waiting on the disk: a two-hop chain."),
	    { knob("asset", QStringLiteral("Asset size"), 8, 1, 128, mb),
	      knob("every", QStringLiteral("Every"), 30, 2, 1000, QStringLiteral(" frames")) });
	add("iodisk", QStringLiteral("Another program reads from the same slow disk"),
	    QStringLiteral("The game streams small assets while another program reads a large file in bursts. Both "
			   "share a disk limited to a set read rate, so they compete for it."),
	    QStringLiteral("The same chain as iothread, but 50 to 100 ms long during the other program's bursts. "
			   "That program is never named: the chain ends at the disk."),
	    { knob("rate", QStringLiteral("Disk read limit"), 200, 10, 5000, QStringLiteral(" MB/s")),
	      knob("asset", QStringLiteral("Asset size"), 2, 1, 128, mb),
	      knob("every", QStringLiteral("Every"), 15, 2, 1000, QStringLiteral(" frames")),
	      knob("period", QStringLiteral("Other program reads every"), 1500, 100, 10000, ms),
	      knob("burst", QStringLiteral("for"), 400, 10, 5000, ms) });
	add("mem", QStringLiteral("Touching memory that was swapped out"),
	    QStringLiteral("The game keeps a world in memory under a memory limit smaller than the world, so part of "
			   "it sits in swap. Now and then it walks a part it hasn't used for a while."),
	    QStringLiteral("Dozens of short disk waits on the main thread inside a page fault (do_swap_page), plus "
			   "kernel time for the faults."),
	    { knob("limit", QStringLiteral("Memory limit"), 420, 128, 8192, mb),
	      knob("world", QStringLiteral("World size"), 512, 64, 16384, mb),
	      knob("walk", QStringLiteral("Walks"), 2, 1, 256, mb),
	      knob("every", QStringLiteral("Every"), 30, 2, 1000, QStringLiteral(" frames")),
	      toggle("thp", QStringLiteral("Ask for huge pages for the world"), false) });
	add("alloc", QStringLiteral("New memory while memory is full"),
	    QStringLiteral("The game's world fills its memory limit. Now and then it allocates new memory, and the "
			   "kernel has to push something out before it can hand it over (direct reclaim)."),
	    QStringLiteral("The main thread running in the kernel: reclaim. It is not among the waits; look at "
			   "where the frame went."),
	    { knob("limit", QStringLiteral("Memory limit"), 420, 128, 8192, mb),
	      knob("world", QStringLiteral("World size"), 512, 64, 16384, mb),
	      knob("alloc", QStringLiteral("Allocates"), 8, 1, 256, mb),
	      knob("every", QStringLiteral("Every"), 15, 2, 1000, QStringLiteral(" frames")) });
	add("compact", QStringLiteral("Huge pages while free memory is in pieces"),
	    QStringLiteral("Another program holds the machine's free memory in scattered single pages, and the game "
			   "grows with huge pages. The kernel has to move memory to make each one."),
	    QStringLiteral("The main thread's kernel time split into compaction, reclaim and page faults. Hitches "
			   "start once free memory runs out, after 10 to 20 seconds."),
	    { knob("grow", QStringLiteral("Grows by"), 24, 2, 256, mb),
	      knob("every", QStringLiteral("Every"), 15, 2, 1000, QStringLiteral(" frames")),
	      knob("leave", QStringLiteral("Leaves free"), 64, 0, 4096, mb),
	      knob("seconds", QStringLiteral("Stops by itself after"), 30, 10, 120, QStringLiteral(" s")) },
	    QStringLiteral("This case uses up nearly all of the machine's free memory, so the whole desktop slows "
			   "down while it runs. It stops by itself."));
	add("slack", QStringLiteral("The frame limiter's sleep ends late"),
	    QStringLiteral("In bursts, the game's sleeps are made imprecise (timer slack), so the frame limiter "
			   "wakes late, as when the kernel's timers misbehave."),
	    QStringLiteral("A longer timer sleep than in other frames. hitchtrace cannot tell a late sleep from an "
			   "intended one; compare frames."),
	    { knob("slack", QStringLiteral("Wakes up to"), 8, 1, 100, QStringLiteral(" ms late")),
	      knob("period", QStringLiteral("Every"), 1500, 100, 10000, ms),
	      knob("burst", QStringLiteral("for"), 400, 10, 5000, ms) });
	add("ipc", QStringLiteral("Calls to a server that can't get its CPU"),
	    QStringLiteral("Every frame the game makes blocking calls to a server process, as a game under Wine "
			   "calls wineserver. In bursts, a background program crowds the server's one CPU."),
	    QStringLiteral("The main thread waited for mg_server (another process), which was kept off its CPU. Who "
			   "had that CPU is not recorded."),
	    { knob("calls", QStringLiteral("Calls per frame"), 30, 1, 500),
	      knob("threads", QStringLiteral("Background threads"), 4, 1, 16),
	      knob("period", QStringLiteral("Every"), 700, 50, 10000, ms),
	      knob("burst", QStringLiteral("for"), 25, 1, 1000, ms) });
	add("cpuspike", QStringLiteral("The game's own CPU work"),
	    QStringLiteral("Now and then the main thread does extra CPU work of its own, like a shader compile."),
	    QStringLiteral("The main thread running, and no wait blamed: a job for a CPU profiler."),
	    { knob("burn", QStringLiteral("Extra work"), 12, 1, 500, ms),
	      knob("every", QStringLiteral("Every"), 30, 2, 1000, QStringLiteral(" frames")) });
	add("squeeze", QStringLiteral("The game's threads confined to one core"),
	    QStringLiteral("In bursts, all of the game's threads may run on one core only, while the others sit "
			   "idle, like a scheduler packing them badly or a wrong affinity mask."),
	    QStringLiteral("Workers taking turns, each waiting for a CPU behind another of the game's threads. "
			   "Which core is not recorded."),
	    { knob("period", QStringLiteral("Every"), 1500, 200, 10000, ms),
	      knob("burst", QStringLiteral("for"), 300, 10, 5000, ms) });
	return cs;
}

LabPlan planFor(const LabCase &c, const QMap<QString, int> &v, const LabEnv &env)
{
	LabPlan plan;
	const QList<int> game = parseCpus(env.gameCpus);
	if (game.isEmpty() || game.last() >= env.cpus) {
		plan.problem = QStringLiteral("The game's CPUs (%1) are not CPUs this machine has (0-%2).")
				       .arg(env.gameCpus).arg(env.cpus - 1);
		return plan;
	}
	QList<int> others;
	for (int i = 0; i < env.cpus; i++)
		if (!game.contains(i))
			others << i;
	auto val = [&](const char *k) { return v.value(QLatin1String(k)); };

	plan.game.name = QStringLiteral("game");
	plan.game.cpus = cpuText(game);
	plan.game.args = { QStringLiteral("-d"), QStringLiteral("86400"), QStringLiteral("-w"), QStringLiteral("3"),
			   QStringLiteral("-n"), QStringLiteral("120"), QStringLiteral("-T"),
			   QStringLiteral("Hitch Lab: %1").arg(c.key) };
	auto helper = [&](const char *name, QStringList args, const QString &cpus = {}) {
		LabProc p;
		p.name = QLatin1String(name);
		p.args = args;
		p.cpus = cpus;
		plan.helpers << p;
		return &plan.helpers.last();
	};
	const QString longRun = QStringLiteral("86400");
	const QString k = c.key;

	if (k == QLatin1String("bgcpu")) {
		helper("bgload", { "-t", num(val("threads")), "-p", num(val("period")), "-b", num(val("burst")), "-d", longRun },
		       plan.game.cpus);
	} else if (k == QLatin1String("iomain") || k == QLatin1String("iothread")) {
		plan.needsPack = true;
		plan.game.args << "-s" << (k == QLatin1String("iomain") ? "main" : "thread") << "-p" << packPath(env) << "-a"
			       << num(val("asset")) << "-e" << num(val("every"));
	} else if (k == QLatin1String("iodisk")) {
		if (!ioDelegated()) {
			plan.problem = QStringLiteral(
				"This case needs the io cgroup controller delegated to your user, once, as root:<br>"
				"<code>sudo mkdir -p /etc/systemd/system/user@.service.d && printf '[Service]\\nDelegate=cpu "
				"cpuset io memory pids\\n' | sudo tee /etc/systemd/system/user@.service.d/delegate.conf && "
				"sudo systemctl daemon-reload</code><br>then log out and in (or run "
				"<code>systemctl --user daemon-reexec</code>).");
			return plan;
		}
		plan.needsPack = plan.needsScratch = true;
		const QString slice = QStringLiteral("hitchlab-disk.slice");
		plan.setup << QStringList{ "systemctl", "--user", "set-property", "--runtime", slice,
					   QStringLiteral("IOReadBandwidthMax=%1 %2M").arg(env.dataDir).arg(val("rate")) };
		const QStringList wrap = { "systemd-run", "--user", "--scope", "-q", "--slice=" + slice };
		plan.game.wrap = wrap;
		plan.game.args << "-s" << "thread" << "-p" << packPath(env) << "-a" << num(val("asset")) << "-e" << num(val("every"));
		LabProc *io = helper("bgio", { "-R", "-f", scratchPath(env), "-t", "4", "-p", num(val("period")), "-b",
					      num(val("burst")), "-d", longRun },
				     cpuText(others.isEmpty() ? game : others));
		io->wrap = wrap;
	} else if (k == QLatin1String("mem")) {
		plan.game.wrap = { "systemd-run", "--user", "--scope", "-q", "-p", QStringLiteral("MemoryMax=%1M").arg(val("limit")) };
		plan.game.args << "-m" << num(val("world")) << "-g" << num(val("walk")) << "-e" << num(val("every"));
		if (val("thp"))
			plan.game.args << "-H";
	} else if (k == QLatin1String("alloc")) {
		plan.game.wrap = { "systemd-run", "--user", "--scope", "-q", "-p", QStringLiteral("MemoryMax=%1M").arg(val("limit")) };
		plan.game.args << "-m" << num(val("world")) << "-g" << "0" << "-A" << num(val("alloc")) << "-K" << "64" << "-e"
			       << num(val("every"));
	} else if (k == QLatin1String("compact")) {
		LabProc *f = helper("frag", { "-l", num(val("leave")) });
		f->beforeGame = true;
		f->readyText = QStringLiteral("ready");
		plan.game.args << "-A" << num(val("grow")) << "-K" << "1024" << "-H" << "-e" << num(val("every"));
		plan.stopAfter = val("seconds");
	} else if (k == QLatin1String("slack")) {
		plan.game.env << "LD_PRELOAD=" + env.slackLib << QStringLiteral("SLACK_NS=%1").arg(qint64(val("slack")) * 1000000)
			      << "SLACK_PERIOD_MS=" + num(val("period")) << "SLACK_BURST_MS=" + num(val("burst"));
		if (env.slackLib.isEmpty())
			plan.problem = QStringLiteral("libhitchlab_slack.so is missing next to hitchlab: rebuild the lab.");
	} else if (k == QLatin1String("ipc")) {
		const int server = others.isEmpty() ? env.cpus - 1 : others.first();
		const QString sock = env.dataDir + QStringLiteral("/server.sock");
		LabProc *s = helper("ipcserver", { sock }, num(server));
		s->beforeGame = true;
		s->readyText = QStringLiteral("socket=");
		plan.game.args << "-i" << sock << "-c" << num(val("calls"));
		helper("bgload", { "-t", num(val("threads")), "-p", num(val("period")), "-b", num(val("burst")), "-d", longRun },
		       num(server));
	} else if (k == QLatin1String("cpuspike")) {
		plan.game.args << "-b" << num(val("burn")) << "-e" << num(val("every"));
	} else if (k == QLatin1String("squeeze")) {
		LabProc *s = helper("squeeze", { "-c", num(game.first()), "-p", num(val("period")), "-b", num(val("burst")), "-d",
						 longRun });
		s->wantsGamePid = true;
	}
	return plan;
}

}  // namespace ht
