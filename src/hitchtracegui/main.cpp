// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// hitchtracegui: look at hitchtrace recordings.
//
//   hitchtracegui [--frame ID] [FILE.jsonl ...]
//   hitchtracegui --record PID       open the Record dialog on that process
//
// Each file is one recording (`hitchtrace -o FILE`); the last one opened is
// shown first. --screenshot FILE renders the window to an image and exits,
// for documentation and for checking the views without a display
// (QT_QPA_PLATFORM=offscreen).
#include "ui/main_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("hitchtracegui"));
	QApplication::setOrganizationName(QStringLiteral("hitchtrace"));
	QApplication::setApplicationDisplayName(QStringLiteral("hitchtracegui"));

	QCommandLineParser cli;
	cli.setApplicationDescription(QStringLiteral("Look at hitchtrace recordings."));
	cli.addHelpOption();
	cli.addPositionalArgument(QStringLiteral("files"), QStringLiteral("hitch.jsonl files hitchtrace wrote"),
				  QStringLiteral("[FILE.jsonl ...]"));
	QCommandLineOption frame(QStringLiteral("frame"), QStringLiteral("Open this frame of the first file."),
				 QStringLiteral("ID"));
	QCommandLineOption shot(QStringLiteral("screenshot"),
				QStringLiteral("Render the window to FILE and exit."), QStringLiteral("FILE"));
	QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Window size for --screenshot, WxH."),
				QStringLiteral("WxH"), QStringLiteral("1500x950"));
	QCommandLineOption record(QStringLiteral("record"),
				  QStringLiteral("Open the Record dialog with process PID selected."), QStringLiteral("PID"));
	cli.addOptions({ frame, shot, size, record });
	cli.process(app);

	ht::Session session;
	ht::MainWindow win(&session);
	const QStringList files = cli.positionalArguments();
	if (!files.isEmpty() && win.openFiles(files) > 1)
		session.setRun(0);
	if (cli.isSet(frame))
		win.showFrameId(cli.value(frame).toULongLong());

	if (cli.isSet(shot)) {
		const QStringList wh = cli.value(size).split(QLatin1Char('x'));
		if (wh.size() == 2)
			win.resize(wh[0].toInt(), wh[1].toInt());
		win.show();
		QTimer::singleShot(300, &app, [&] {
			const bool ok = win.grab().save(cli.value(shot));
			QApplication::exit(ok ? 0 : 1);
		});
		return app.exec();
	}
	win.show();
	if (cli.isSet(record))
		QTimer::singleShot(0, &win, [&] { win.recordDialog(cli.value(record).toInt()); });
	return app.exec();
}
