// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// hitchlab: the window, or one of the programs a case runs.
//
//   hitchlab                         the window: pick a case and start it
//   hitchlab --run game ARGS...      the test game (cases/minigame)
//   hitchlab --run bgload ARGS...    a background program that burns CPU
//   hitchlab --run bgio ARGS...      ... that reads or writes a file
//   hitchlab --run ipcserver ARGS... the server the game calls
//   hitchlab --run squeeze ARGS...   confines another process to few CPUs
//   hitchlab --run frag ARGS...      leaves free memory in single pages
//
// For checking the lab without clicking:
//   hitchlab --case KEY --screenshot FILE     render the window and exit
//   hitchlab --case KEY --try SECONDS         start the case, print its output,
//                                             stop it after SECONDS; exit 1 if
//                                             the game was not running by then
//
// The window starts the others as `hitchlab --run ...`, so one file is the
// whole lab (plus the timer-slack shim it preloads into the game).
#include "lab_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <sys/prctl.h>

extern "C" {
int game_main(int argc, char **argv);
int bgload_main(int argc, char **argv);
int bgio_main(int argc, char **argv);
int ipcserver_main(int argc, char **argv);
int squeeze_main(int argc, char **argv);
int frag_main(int argc, char **argv);
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !std::strcmp(argv[1], "--run")) {
		static const struct {
			const char *name;
			int (*run)(int, char **);
		} programs[] = {
			{ "game", game_main }, { "bgload", bgload_main }, { "bgio", bgio_main },
			{ "ipcserver", ipcserver_main }, { "squeeze", squeeze_main }, { "frag", frag_main },
		};
		for (const auto &p : programs) {
			if (std::strcmp(argv[2], p.name))
				continue;
			// named after what it is, not after this binary: hitchtrace shows
			// these names (the game names its own threads)
			if (std::strcmp(p.name, "game"))
				prctl(PR_SET_NAME, p.name);
			return p.run(argc - 2, argv + 2);
		}
		std::fprintf(stderr, "hitchlab: no program called %s\n", argv[2]);
		return 2;
	}

	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("hitchlab"));
	QApplication::setOrganizationName(QStringLiteral("hitchtrace"));
	QApplication::setApplicationDisplayName(QStringLiteral("Hitch Lab"));
	QCommandLineParser cli;
	cli.setApplicationDescription(QStringLiteral("The case-study game, with switchable causes of hitches."));
	cli.addHelpOption();
	QCommandLineOption caseOpt(QStringLiteral("case"), QStringLiteral("Select this case."), QStringLiteral("KEY"));
	QCommandLineOption shot(QStringLiteral("screenshot"), QStringLiteral("Render the window to FILE and exit."),
				QStringLiteral("FILE"));
	QCommandLineOption tryOpt(QStringLiteral("try"), QStringLiteral("Run the case for SECONDS, then stop and exit."),
				  QStringLiteral("SECONDS"));
	cli.addOptions({ caseOpt, shot, tryOpt });
	cli.process(app);

	ht::LabWindow win;
	if (cli.isSet(caseOpt) && !win.selectCase(cli.value(caseOpt))) {
		std::fprintf(stderr, "hitchlab: no case called %s\n", qPrintable(cli.value(caseOpt)));
		return 2;
	}
	win.show();
	if (cli.isSet(shot)) {
		QTimer::singleShot(300, &app, [&] { QApplication::exit(win.grab().save(cli.value(shot)) ? 0 : 1); });
		return app.exec();
	}
	if (cli.isSet(tryOpt)) {
		const int secs = cli.value(tryOpt).toInt();
		int rc = 1;
		QTimer::singleShot(0, &win, &ht::LabWindow::start);
		QTimer::singleShot(secs * 1000, &win, [&] {
			rc = win.running() ? 0 : 1;
			std::printf("game %s after %d s (process %lld)\n", rc ? "NOT running" : "running", secs,
				    (long long)win.gamePid());
			win.stop();
		});
		QTimer::singleShot(secs * 1000 + 5000, &app, [&] { QApplication::exit(rc); });
		app.exec();
		return rc;
	}
	return app.exec();
}
