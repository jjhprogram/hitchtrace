// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// Drives the main window offscreen through what a person does with it, on
// each case recording that exists: open a frame, follow one wait, select a
// range, look at all hitches zoomed one level in. Saves a screenshot of every
// state into the directory given (default gui-smoke/) and fails if a view has
// nothing to show where it should.
//
//   gui_smoke [DIR]                 the views, and the Record dialog's first page
//   gui_smoke DIR --record PID      also record PID for real: press Record, wait,
//                                   press Stop, and check the recording opened.
//                                   Needs PID running with the hitchtrace layer
//                                   and hitchtrace runnable as root (sudo -n)
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/record_dialog.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPalette>
#include <QStyleFactory>
#include <cstdio>

using namespace ht;

int main(int argc, char **argv)
{
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	// a settings file of its own: the window's saved layout must not leak in
	QApplication::setOrganizationName(QStringLiteral("hitchtrace-test"));
	QApplication::setApplicationName(QStringLiteral("gui-smoke"));
	const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("gui-smoke");
	const int recordPid = argc > 3 && QByteArray(argv[2]) == "--record" ? atoi(argv[3]) : 0;
	QDir().mkpath(out);

	struct Case { const char *dir; std::uint64_t frame; };
	const Case cases[] = {
		{ "bgcpu-timeline", 2241 }, { "iothread-timeline", 2129 }, { "iodisk", 2144 }, { "mem", 779 },
		{ "slack", 1030 }, { "ipc", 2475 }, { "cpuspike", 2009 }, { "squeeze", 540 }, { "alloc", 1604 },
		{ "compact", 1514 },
	};
	Session session;
	MainWindow win(&session);
	win.resize(1500, 950);
	win.show();
	int failures = 0, shots = 0;
	auto shot = [&](const QString &name) {
		for (int i = 0; i < 3; i++)
			QCoreApplication::processEvents();
		if (!win.grab().save(out + QLatin1Char('/') + name + QStringLiteral(".png"))) {
			std::fprintf(stderr, "FAIL: cannot save %s\n", qPrintable(name));
			failures++;
		}
		shots++;
	};
	auto expect = [&](bool ok, const char *what, const char *run) {
		if (!ok) {
			std::fprintf(stderr, "FAIL: %s: %s\n", run, what);
			failures++;
		}
	};

	for (const auto &c : cases) {
		const QString path = QStringLiteral(CASES_DIR "/%1/hitch.jsonl").arg(QLatin1String(c.dir));
		if (!QFileInfo::exists(path)) {
			std::printf("skip %s: no recording\n", c.dir);
			continue;
		}
		if (win.openFiles({ path }) != 1) {
			expect(false, "does not open", c.dir);
			continue;
		}
		expect(win.showFrameId(c.frame), "frame not found", c.dir);
		expect(!session.activeSegments().empty(), "no critical path", c.dir);
		expect(session.scopeTree() && session.scopeTree()->ns == session.frame()->duration,
		       "frame blame tree does not add up to the frame", c.dir);
		shot(QStringLiteral("%1-frame").arg(QLatin1String(c.dir)));

		// zoom the thread lanes 4x into the frame's middle, then back out
		if (auto *lanes = win.findChild<Lanes *>()) {
			const std::int64_t d = session.frame()->duration;
			lanes->zoomBy(4, d / 2);
			const auto [a, b] = lanes->view();
			expect(lanes->zoomed() && std::llabs((b - a) - d / 4) <= 1 && a <= d / 2 && b >= d / 2,
			       "zoom does not show a quarter of the frame around its middle", c.dir);
			shot(QStringLiteral("%1-zoom").arg(QLatin1String(c.dir)));
			lanes->resetZoom();
			expect(!lanes->zoomed(), "zoom does not reset", c.dir);
		} else {
			expect(false, "no thread lanes", c.dir);
		}

		// follow the presenting thread's longest wait
		const Run &run = *session.run();
		const Frame &f = *session.frame();
		const Event *longest = nullptr;
		for (const Event *e : threadEvents(run, f, f.rootTid))
			if (e->cause != Cause::Runnable && e->cause != Cause::Open
			    && (!longest || e->wake - e->out > longest->wake - longest->out))
				longest = e;
		if (longest) {
			session.followWait(longest);
			expect(session.scope() == Session::Scope::Wait && session.scopeTree(), "no wait tree", c.dir);
			shot(QStringLiteral("%1-wait").arg(QLatin1String(c.dir)));
			session.followWait(nullptr);
		}

		// a range over the middle third of the run
		session.setRange(run.span / 3, run.span * 2 / 3);
		expect(session.scope() == Session::Scope::Selection && !session.rangeFrames().empty(),
		       "range selected nothing", c.dir);
		shot(QStringLiteral("%1-range").arg(QLatin1String(c.dir)));
		session.clearRange();

		// all hitches, zoomed into the largest part
		session.setScope(Session::Scope::All);
		const BlameNode *all = session.scopeTree();
		expect(all && all->frameCount == int(run.frames.size()), "all-hitches tree", c.dir);
		if (all && !all->kids.empty())
			session.setZoom({ all->sortedKids().front()->key });
		shot(QStringLiteral("%1-all").arg(QLatin1String(c.dir)));
		session.setScope(Session::Scope::Frame);
		std::printf("ok   %s\n", c.dir);
	}
	// the same views in a dark palette: colours come from the palette, not literals
	if (!session.runs().empty()) {
		QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
		QPalette dark;
		dark.setColor(QPalette::Window, QColor(0x1f, 0x1f, 0x1e));
		dark.setColor(QPalette::WindowText, QColor(0xe8, 0xe8, 0xe4));
		dark.setColor(QPalette::Base, QColor(0x1a, 0x1a, 0x19));
		dark.setColor(QPalette::AlternateBase, QColor(0x24, 0x24, 0x23));
		dark.setColor(QPalette::Text, QColor(0xf4, 0xf4, 0xf1));
		dark.setColor(QPalette::Button, QColor(0x2a, 0x2a, 0x28));
		dark.setColor(QPalette::ButtonText, QColor(0xe8, 0xe8, 0xe4));
		dark.setColor(QPalette::Highlight, QColor(0x39, 0x87, 0xe5));
		dark.setColor(QPalette::PlaceholderText, QColor(0x8d, 0x8b, 0x84));
		QApplication::setPalette(dark);
		session.setRun(0);
		shot(QStringLiteral("dark-frame"));
	}
	// the Record dialog: its first page always; a real recording when asked
	{
		RecordDialog dlg(&win);
		dlg.resize(860, 720);
		dlg.show();
		if (recordPid)
			expect(dlg.selectPid(recordPid), "the program to record is not listed as ready", "record");
		for (int i = 0; i < 3; i++)
			QCoreApplication::processEvents();
		if (!dlg.grab().save(out + QStringLiteral("/record-setup.png")))
			failures++;
		shots++;
		if (recordPid) {
			QString got;
			QObject::connect(&dlg, &RecordDialog::recorded, [&](const QString &p) { got = p; });
			const int runsBefore = int(session.runs().size());
			dlg.startRecording();
			expect(dlg.recording(), "hitchtrace did not start", "record");
			QElapsedTimer t;
			t.start();
			while (t.elapsed() < 5000)
				QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
			if (!dlg.grab().save(out + QStringLiteral("/record-running.png")))
				failures++;
			shots++;
			dlg.stopRecording();
			t.restart();
			while (dlg.recording() && t.elapsed() < 15000)
				QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
			QCoreApplication::processEvents();
			expect(!dlg.recording(), "hitchtrace did not stop", "record");
			expect(!got.isEmpty(), "no recording came back", "record");
			if (!got.isEmpty()) {
				win.openFiles({ got });
				expect(int(session.runs().size()) == runsBefore + 1 && session.frame(),
				       "the recording did not open", "record");
				shot(QStringLiteral("record-opened"));
				std::printf("ok   recorded %s\n", qPrintable(got));
			}
		}
	}
	std::printf("%d screenshots in %s, %d failures\n", shots, qPrintable(out), failures);
	return failures ? 1 : 0;
}
