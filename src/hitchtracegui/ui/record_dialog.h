// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// Record: pick a running program that has the hitchtrace Vulkan layer loaded,
// choose the frame budget and how long to record, and run hitchtrace on it.
//
// hitchtrace needs root. It is started through `sudo -n` when a rule lets
// this user run it without a password, and otherwise through `pkexec`, which
// asks in the system's own dialog; this program never sees a password. Stop
// closes hitchtrace's standard input (hitchtrace --stop-on-eof), which works
// whatever user it runs as. The recording opens when hitchtrace finishes.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QProcess>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QTreeWidget;

namespace ht {

class RecordDialog : public QDialog {
	Q_OBJECT
public:
	explicit RecordDialog(QWidget *parent = nullptr);
	~RecordDialog() override;

	// For tests: select this process if it is listed.
	bool selectPid(int pid);
	bool recording() const;

public slots:
	void refreshPrograms();
	void startRecording();
	void stopRecording();
	void reject() override;

signals:
	// hitchtrace finished and wrote at least one hitch to @path
	void recorded(const QString &path);

private:
	enum class Root { Sudo, Pkexec, None };

	struct Program {
		int pid = 0;
		QString name, cmdline;
		QString layer;	// the hitchtrace layer as mapped in the process; empty if not loaded
	};

	QWidget *buildSetup();
	QWidget *buildRunning();
	void updateState();
	void checkTracer();
	QStringList tracerArgs() const;
	const Program *selected() const;
	QString defaultOutput(const Program &p) const;
	void onOutput();
	void onFinished(int code, QProcess::ExitStatus status);
	void tick();

	QStackedWidget *pages_;
	// setup
	QLineEdit *filter_;
	QCheckBox *onlyLayer_;
	QTreeWidget *programs_;
	QComboBox *budgetMode_;
	QDoubleSpinBox *budgetMs_;
	QSpinBox *seconds_;
	QCheckBox *userStacks_;
	QLineEdit *output_;
	QLineEdit *tracer_;
	QLabel *rootNote_, *problem_;
	QPlainTextEdit *command_;
	QPushButton *record_;
	// running
	QLabel *runTitle_, *runStatus_;
	QPlainTextEdit *log_;
	QPushButton *stop_, *back_;

	std::vector<Program> list_;
	Root root_ = Root::None;
	QString rootFor_;		// the tracer path root_ was checked for
	QProcess *proc_ = nullptr;
	QTimer *timer_;
	QElapsedTimer clock_;
	QString outPath_, outBuf_;
	int hitches_ = 0;
	int duration_ = 0;
	bool stopping_ = false;
};

}  // namespace ht
