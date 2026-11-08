// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// The lab's window: the cases on the left; on the right what the selected one
// simulates, what to look for, its settings and the exact commands it runs.
// Start runs the game with the case's surroundings; Stop ends all of it.
// Record opens hitchtracegui's Record dialog on the running game.
//
// Every process the lab starts is pinned as the case asks and dies with the
// lab (PR_SET_PDEATHSIG), so nothing is left burning CPU if the lab is killed.
#pragma once

#include "lab_cases.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QProcess>
#include <atomic>
#include <functional>
#include <memory>

class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace ht {

class LabWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit LabWindow(QWidget *parent = nullptr);
	~LabWindow() override;

	// for tests and scripts
	bool selectCase(const QString &key);
	bool running() const { return stage_ == Stage::Running; }
	qint64 gamePid() const;

public slots:
	void start();
	void stop();

protected:
	void closeEvent(QCloseEvent *e) override;

private:
	enum class Stage { Idle, Preparing, Starting, Running, Stopping };

	void showCase(int row);
	const LabCase *current() const;
	QMap<QString, int> values() const;
	LabEnv env() const;
	void refreshPlan();
	void continueStart();
	void startNextBefore(int i);
	void startGame();
	void startAfter();
	QProcess *launch(const LabProc &p, bool isGame, std::function<void()> onStarted = {});
	QString commandText(const LabProc &p, bool isGame) const;
	void onOutput(QProcess *p, const QString &name);
	void onFinished(QProcess *p, const QString &name, int code);
	void finishStop();
	void log(const QString &who, const QString &line);
	void updateStatus();
	void recordGame();
	QString guiPath() const;

	QList<LabCase> cases_;
	QListWidget *list_;
	QLabel *title_, *what_, *look_, *warning_, *problem_, *status_, *machine_;
	QWidget *knobBox_;
	QFormLayout *knobs_;
	QLineEdit *gameCpus_;
	QPlainTextEdit *commands_, *log_;
	QPushButton *start_, *stop_, *record_;
	QTimer *tick_;

	Stage stage_ = Stage::Idle;
	QString runningKey_, runningTitle_;
	QString pendingKey_;	// a case to start once the running one has stopped
	LabPlan plan_;
	QList<QProcess *> procs_;
	QProcess *game_ = nullptr;
	QElapsedTimer clock_;
	std::shared_ptr<std::atomic<bool>> cancelPrepare_;
};

}  // namespace ht
