// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "lab_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <csignal>
#include <cstdio>
#include <sched.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>

namespace ht {

namespace {

constexpr qint64 PackMb = 256, ScratchMb = 1024;

QLabel *wrapped(const QString &text = {})
{
	auto *l = new QLabel(text);
	l->setWordWrap(true);
	l->setTextFormat(Qt::RichText);
	l->setTextInteractionFlags(Qt::TextSelectableByMouse);
	return l;
}

QLabel *heading(const QString &text)
{
	auto *l = new QLabel(text);
	QFont f = l->font();
	f.setBold(true);
	l->setFont(f);
	return l;
}

QString quoted(const QString &s)
{
	bool plain = !s.isEmpty();
	for (QChar c : s)
		plain &= c.isLetterOrNumber() || QStringLiteral("-_./=:+,%@").contains(c);
	if (plain)
		return s;
	QString q = s;
	q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
	return QLatin1Char('\'') + q + QLatin1Char('\'');
}

// A file of real blocks (not sparse: reading it must cost a disk read).
bool makeFile(const QString &path, qint64 mb, const std::atomic<bool> &cancel)
{
	QFile f(path + QStringLiteral(".part"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	const QByteArray block(1 << 20, '\0');
	for (qint64 i = 0; i < mb; i++) {
		if (cancel || f.write(block) != block.size())
			return false;
	}
	f.flush();
	::fsync(f.handle());
	f.close();
	QFile::remove(path);
	return QFile::rename(path + QStringLiteral(".part"), path);
}

bool haveFile(const QString &path, qint64 mb)
{
	return QFileInfo(path).size() == mb << 20;
}

}  // namespace

LabWindow::LabWindow(QWidget *parent) : QMainWindow(parent), cases_(labCases())
{
	setWindowTitle(tr("Hitch Lab"));
	resize(1180, 820);

	auto *split = new QSplitter;
	list_ = new QListWidget;
	for (const auto &c : cases_) {
		auto *it = new QListWidgetItem(c.title, list_);
		it->setToolTip(c.key);
		it->setData(Qt::UserRole, c.key);
	}
	list_->setMinimumWidth(300);
	list_->setWordWrap(true);
	list_->setSpacing(2);
	split->addWidget(list_);

	auto *right = new QWidget;
	auto *v = new QVBoxLayout(right);
	v->setContentsMargins(12, 6, 6, 6);
	title_ = new QLabel;
	QFont tf = title_->font();
	tf.setPointSizeF(tf.pointSizeF() * 1.4);
	tf.setBold(true);
	title_->setFont(tf);
	title_->setWordWrap(true);
	v->addWidget(title_);
	v->addWidget(heading(tr("What it simulates")));
	what_ = wrapped();
	v->addWidget(what_);
	v->addWidget(heading(tr("What to look for in hitchtracegui")));
	look_ = wrapped();
	v->addWidget(look_);
	warning_ = wrapped();
	v->addWidget(warning_);

	auto *settings = new QGroupBox(tr("Settings"));
	auto *sv = new QVBoxLayout(settings);
	knobBox_ = new QWidget;
	knobs_ = new QFormLayout(knobBox_);
	knobs_->setContentsMargins(0, 0, 0, 0);
	sv->addWidget(knobBox_);
	auto *cpuRow = new QFormLayout;
	cpuRow->setContentsMargins(0, 0, 0, 0);
	gameCpus_ = new QLineEdit(QSettings().value(QStringLiteral("gameCpus"), QStringLiteral("0-3")).toString());
	gameCpus_->setToolTip(tr("The game runs pinned to these CPUs, like the case studies. Other programs a case "
				 "runs are placed on or off them as the case needs."));
	cpuRow->addRow(tr("Game's CPUs (all cases)"), gameCpus_);
	sv->addLayout(cpuRow);
	machine_ = new QLabel;
	machine_->setForegroundRole(QPalette::PlaceholderText);
	sv->addWidget(machine_);
	v->addWidget(settings);

	v->addWidget(heading(tr("Will run")));
	commands_ = new QPlainTextEdit;
	commands_->setReadOnly(true);
	commands_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	commands_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	commands_->setMaximumHeight(commands_->fontMetrics().lineSpacing() * 7 + 12);
	v->addWidget(commands_);

	problem_ = wrapped();
	v->addWidget(problem_);
	auto *buttons = new QHBoxLayout;
	start_ = new QPushButton(tr("Start"));
	start_->setDefault(true);
	stop_ = new QPushButton(tr("Stop"));
	record_ = new QPushButton(tr("Record with hitchtracegui…"));
	buttons->addWidget(start_);
	buttons->addWidget(stop_);
	buttons->addStretch(1);
	buttons->addWidget(record_);
	v->addLayout(buttons);
	status_ = wrapped();
	v->addWidget(status_);
	v->addWidget(heading(tr("Output")));
	log_ = new QPlainTextEdit;
	log_->setReadOnly(true);
	log_->setMaximumBlockCount(2000);
	log_->setLineWrapMode(QPlainTextEdit::NoWrap);
	log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	v->addWidget(log_, 1);
	split->addWidget(right);
	split->setStretchFactor(1, 1);
	setCentralWidget(split);

	tick_ = new QTimer(this);
	tick_->setInterval(500);
	connect(tick_, &QTimer::timeout, this, &LabWindow::updateStatus);
	connect(list_, &QListWidget::currentRowChanged, this, &LabWindow::showCase);
	connect(start_, &QPushButton::clicked, this, &LabWindow::start);
	connect(stop_, &QPushButton::clicked, this, &LabWindow::stop);
	connect(record_, &QPushButton::clicked, this, &LabWindow::recordGame);
	connect(gameCpus_, &QLineEdit::textChanged, this, [this] {
		QSettings().setValue(QStringLiteral("gameCpus"), gameCpus_->text());
		refreshPlan();
	});

	const QString last = QSettings().value(QStringLiteral("case"), QStringLiteral("bgcpu")).toString();
	if (!selectCase(last))
		list_->setCurrentRow(0);
	updateStatus();
}

LabWindow::~LabWindow()
{
	if (cancelPrepare_)
		*cancelPrepare_ = true;
	for (QProcess *p : procs_) {
		p->disconnect(this);
		p->terminate();
	}
	for (QProcess *p : procs_)
		if (!p->waitForFinished(2000))
			p->kill();
}

bool LabWindow::selectCase(const QString &key)
{
	for (int i = 0; i < list_->count(); i++)
		if (list_->item(i)->data(Qt::UserRole).toString() == key) {
			list_->setCurrentRow(i);
			return true;
		}
	return false;
}

const LabCase *LabWindow::current() const
{
	const int row = list_->currentRow();
	return row >= 0 && row < cases_.size() ? &cases_[row] : nullptr;
}

void LabWindow::showCase(int row)
{
	if (row < 0 || row >= cases_.size())
		return;
	const LabCase &c = cases_[row];
	QSettings().setValue(QStringLiteral("case"), c.key);
	// the key is the name the case goes by elsewhere (cases/out, run.sh, the window title)
	title_->setText(QStringLiteral("%1 <span style=\"font-weight:normal; font-size:small\">· %2</span>")
				.arg(c.title.toHtmlEscaped(), c.key));
	what_->setText(c.what.toHtmlEscaped());
	look_->setText(c.look.toHtmlEscaped());
	warning_->setText(c.warning.isEmpty() ? QString() : QStringLiteral("<b>%1</b>").arg(c.warning.toHtmlEscaped()));
	warning_->setVisible(!c.warning.isEmpty());

	while (knobs_->rowCount())
		knobs_->removeRow(0);
	QSettings s;
	for (const Knob &k : c.knobs) {
		const QString key = QStringLiteral("cases/%1/%2").arg(c.key, k.key);
		const int value = s.value(key, k.value).toInt();
		if (k.toggle) {
			auto *cb = new QCheckBox(k.label);
			cb->setObjectName(k.key);
			cb->setChecked(value);
			connect(cb, &QCheckBox::toggled, this, [this, key](bool on) {
				QSettings().setValue(key, on ? 1 : 0);
				refreshPlan();
			});
			knobs_->addRow(QString(), cb);
		} else {
			auto *sb = new QSpinBox;
			sb->setObjectName(k.key);
			sb->setRange(k.min, k.max);
			sb->setValue(value);
			sb->setSuffix(k.suffix);
			if (!k.help.isEmpty())
				sb->setToolTip(k.help);
			connect(sb, &QSpinBox::valueChanged, this, [this, key](int x) {
				QSettings().setValue(key, x);
				refreshPlan();
			});
			knobs_->addRow(k.label, sb);
		}
	}
	if (c.knobs.isEmpty())
		knobs_->addRow(new QLabel(tr("This case has no settings.")));
	refreshPlan();
}

QMap<QString, int> LabWindow::values() const
{
	QMap<QString, int> v;
	const LabCase *c = current();
	if (!c)
		return v;
	for (const Knob &k : c->knobs) {
		if (auto *sb = knobBox_->findChild<QSpinBox *>(k.key))
			v[k.key] = sb->value();
		else if (auto *cb = knobBox_->findChild<QCheckBox *>(k.key))
			v[k.key] = cb->isChecked();
		else
			v[k.key] = k.value;
	}
	return v;
}

LabEnv LabWindow::env() const
{
	LabEnv e;
	e.self = QCoreApplication::applicationFilePath();
	const QString slack = QCoreApplication::applicationDirPath() + QStringLiteral("/libhitchlab_slack.so");
	e.slackLib = QFileInfo::exists(slack) ? slack : QString();
	e.dataDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
	e.gameCpus = gameCpus_->text().trimmed();
	e.cpus = int(::sysconf(_SC_NPROCESSORS_ONLN));
	return e;
}

QString LabWindow::commandText(const LabProc &p, bool isGame) const
{
	QStringList parts;
	QStringList env = p.env;
	if (isGame)
		env.prepend(QStringLiteral("HITCHTRACE=1"));
	for (const auto &e : env)
		parts << quoted(e);
	if (!p.cpus.isEmpty())
		parts << QStringLiteral("taskset -c") << p.cpus;
	for (const auto &w : p.wrap)
		parts << quoted(w);
	parts << QStringLiteral("hitchlab --run") << p.name;
	for (const auto &a : p.args)
		parts << quoted(a);
	if (p.wantsGamePid)
		parts << QStringLiteral("-P <the game's process id>");
	return parts.join(QLatin1Char(' '));
}

void LabWindow::refreshPlan()
{
	const LabCase *c = current();
	if (!c)
		return;
	const LabEnv e = env();
	machine_->setText(tr("This machine has %1 CPUs (0-%2).").arg(e.cpus).arg(e.cpus - 1));
	const LabPlan plan = planFor(*c, values(), e);
	problem_->setText(plan.problem);
	problem_->setVisible(!plan.problem.isEmpty());
	QStringList lines;
	for (const auto &cmd : plan.setup) {
		QStringList q;
		for (const auto &a : cmd)
			q << quoted(a);
		lines << q.join(QLatin1Char(' '));
	}
	for (const auto &h : plan.helpers)
		if (h.beforeGame)
			lines << commandText(h, false);
	if (plan.problem.isEmpty())
		lines << commandText(plan.game, true);
	for (const auto &h : plan.helpers)
		if (!h.beforeGame)
			lines << commandText(h, false);
	if (plan.needsPack && !haveFile(packPath(e), PackMb))
		lines.prepend(tr("# first, once: write a %1 MB asset pack to %2").arg(PackMb).arg(packPath(e)));
	if (plan.needsScratch && !haveFile(scratchPath(e), ScratchMb))
		lines.prepend(tr("# first, once: write a %1 MB file for the other program to read, %2").arg(ScratchMb).arg(scratchPath(e)));
	if (plan.stopAfter)
		lines << tr("# all of it stops after %1 s").arg(plan.stopAfter);
	commands_->setPlainText(lines.join(QLatin1Char('\n')));
	updateStatus();
}

qint64 LabWindow::gamePid() const
{
	return game_ && game_->state() == QProcess::Running ? game_->processId() : 0;
}

void LabWindow::start()
{
	const LabCase *c = current();
	if (!c)
		return;
	if (stage_ != Stage::Idle) {
		// switching cases: stop the running one; finishStop() starts this one
		pendingKey_ = c->key;
		stop();
		return;
	}
	const LabEnv e = env();
	plan_ = planFor(*c, values(), e);
	if (!plan_.problem.isEmpty()) {
		problem_->setText(plan_.problem);
		problem_->show();
		return;
	}
	runningKey_ = c->key;
	runningTitle_ = c->title;
	log_->clear();
	QDir().mkpath(e.dataDir);

	// the files a case reads, written once and kept
	QList<std::pair<QString, qint64>> make;
	if (plan_.needsPack && !haveFile(packPath(e), PackMb))
		make.push_back({ packPath(e), PackMb });
	if (plan_.needsScratch && !haveFile(scratchPath(e), ScratchMb))
		make.push_back({ scratchPath(e), ScratchMb });
	if (make.isEmpty()) {
		continueStart();
		return;
	}
	stage_ = Stage::Preparing;
	updateStatus();
	cancelPrepare_ = std::make_shared<std::atomic<bool>>(false);
	auto cancel = cancelPrepare_;
	std::thread([this, make, cancel] {
		bool ok = true;
		for (const auto &[path, mb] : make)
			ok = ok && makeFile(path, mb, *cancel);
		if (*cancel)
			return;
		QMetaObject::invokeMethod(this, [this, ok] {
			if (stage_ != Stage::Preparing)
				return;
			if (!ok) {
				stage_ = Stage::Idle;
				problem_->setText(tr("Could not write the files this case reads in %1.").arg(env().dataDir));
				problem_->show();
				updateStatus();
				return;
			}
			continueStart();
		});
	}).detach();
}

void LabWindow::continueStart()
{
	stage_ = Stage::Starting;
	updateStatus();
	for (const QStringList &cmd : plan_.setup) {
		log(QStringLiteral("lab"), cmd.join(QLatin1Char(' ')));
		const int rc = QProcess::execute(cmd[0], cmd.mid(1));
		if (rc != 0) {
			log(QStringLiteral("lab"), tr("that failed (exit code %1); not starting").arg(rc));
			stage_ = Stage::Idle;
			updateStatus();
			return;
		}
	}
	startNextBefore(0);
}

// the programs that must be ready before the game starts, one after another
void LabWindow::startNextBefore(int i)
{
	if (stage_ != Stage::Starting)
		return;
	for (; i < plan_.helpers.size(); i++) {
		const LabProc &h = plan_.helpers[i];
		if (!h.beforeGame)
			continue;
		QProcess *p = launch(h, false);
		if (h.readyText.isEmpty())
			continue;
		auto *conn = new QMetaObject::Connection;
		auto *timeout = new QTimer(this);
		timeout->setSingleShot(true);
		*conn = connect(p, &QProcess::readyRead, this, [this, p, i, conn, timeout] {
			if (!p->property("seen").toString().contains(plan_.helpers[i].readyText))
				return;
			disconnect(*conn);
			delete conn;
			timeout->deleteLater();
			startNextBefore(i + 1);
		});
		connect(timeout, &QTimer::timeout, this, [this, i] {
			if (stage_ != Stage::Starting)
				return;
			log(QStringLiteral("lab"), tr("%1 did not get ready; stopping").arg(plan_.helpers[i].name));
			stop();
		});
		timeout->start(30000);
		return;	// continues from the ready signal
	}
	startGame();
}

void LabWindow::startGame()
{
	clock_.start();
	tick_->start();
	game_ = launch(plan_.game, true, [this] {
		stage_ = Stage::Running;
		updateStatus();
		// the rest once the game is up and has its threads
		QTimer::singleShot(1500, this, &LabWindow::startAfter);
	});
}

void LabWindow::startAfter()
{
	if (stage_ != Stage::Running)
		return;
	for (const LabProc &h : plan_.helpers) {
		if (h.beforeGame)
			continue;
		LabProc p = h;
		if (p.wantsGamePid) {
			p.args << QStringLiteral("-P") << QString::number(gamePid());
			p.wantsGamePid = false;
		}
		launch(p, false);
	}
}

QProcess *LabWindow::launch(const LabProc &lp, bool isGame, std::function<void()> onStarted)
{
	auto *p = new QProcess(this);
	p->setProcessChannelMode(QProcess::MergedChannels);
	QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
	if (isGame)
		pe.insert(QStringLiteral("HITCHTRACE"), QStringLiteral("1"));
	for (const QString &kv : lp.env) {
		const int eq = kv.indexOf(QLatin1Char('='));
		pe.insert(kv.left(eq), kv.mid(eq + 1));
	}
	p->setProcessEnvironment(pe);
	QStringList args = lp.wrap.mid(1);
	if (!lp.wrap.isEmpty())
		args << QCoreApplication::applicationFilePath();
	args << QStringLiteral("--run") << lp.name << lp.args;
	p->setProgram(lp.wrap.isEmpty() ? QCoreApplication::applicationFilePath() : lp.wrap[0]);
	p->setArguments(args);

	cpu_set_t set;
	CPU_ZERO(&set);
	const QList<int> cpus = parseCpus(lp.cpus);
	for (int c : cpus)
		CPU_SET(c, &set);
	const bool pin = !cpus.isEmpty();
	p->setChildProcessModifier([set, pin] {
		if (pin)
			sched_setaffinity(0, sizeof(set), &set);
		prctl(PR_SET_PDEATHSIG, SIGTERM);	// never outlive the lab
	});

	const QString name = isGame ? QStringLiteral("game") : lp.name;
	connect(p, &QProcess::readyRead, this, [this, p, name] { onOutput(p, name); });
	if (onStarted)
		connect(p, &QProcess::started, this, onStarted);
	connect(p, &QProcess::finished, this, [this, p, name](int code) { onFinished(p, name, code); });
	connect(p, &QProcess::errorOccurred, this, [this, p, name](QProcess::ProcessError e) {
		if (e == QProcess::FailedToStart) {
			log(name, tr("could not start: %1").arg(p->errorString()));
			stop();
		}
	});
	procs_ << p;
	log(QStringLiteral("lab"), commandText(lp, isGame));
	p->start();
	return p;
}

void LabWindow::onOutput(QProcess *p, const QString &name)
{
	const QString text = QString::fromLocal8Bit(p->readAll());
	// what it said lately, for the readiness check
	p->setProperty("seen", (p->property("seen").toString() + text).right(4096));
	for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
		log(name, line);
}

void LabWindow::onFinished(QProcess *p, const QString &name, int code)
{
	onOutput(p, name);
	if (stage_ == Stage::Stopping) {
		finishStop();
		return;
	}
	log(name, code ? tr("exited with code %1").arg(code) : tr("exited"));
	if (p == game_ && (stage_ == Stage::Running || stage_ == Stage::Starting)) {
		log(QStringLiteral("lab"), tr("the game is gone; stopping the rest"));
		stop();
	}
}

void LabWindow::stop()
{
	if (stage_ == Stage::Idle || stage_ == Stage::Stopping)
		return;
	if (cancelPrepare_)
		*cancelPrepare_ = true;
	stage_ = Stage::Stopping;
	updateStatus();
	for (QProcess *p : procs_)
		if (p->state() != QProcess::NotRunning)
			p->terminate();
	QTimer::singleShot(3000, this, [this] {
		for (QProcess *p : procs_)
			if (p->state() != QProcess::NotRunning)
				p->kill();
	});
	finishStop();
}

void LabWindow::finishStop()
{
	for (QProcess *p : procs_)
		if (p->state() != QProcess::NotRunning)
			return;
	for (QProcess *p : procs_)
		p->deleteLater();
	procs_.clear();
	game_ = nullptr;
	QFile::remove(env().dataDir + QStringLiteral("/server.sock"));
	stage_ = Stage::Idle;
	tick_->stop();
	log(QStringLiteral("lab"), tr("stopped"));
	updateStatus();
	if (!pendingKey_.isEmpty()) {
		const QString key = pendingKey_;
		pendingKey_.clear();
		if (selectCase(key))
			start();
	}
}

void LabWindow::log(const QString &who, const QString &line)
{
	const QString text = QStringLiteral("%1 %2").arg(who + QLatin1Char(':'), -10).arg(line);
	log_->appendPlainText(text);
	if (qEnvironmentVariableIsSet("HITCHLAB_ECHO"))
		std::printf("%s\n", qPrintable(text));
}

void LabWindow::updateStatus()
{
	const LabCase *c = current();
	const bool problem = !problem_->text().isEmpty();
	const bool here = c && c->key == runningKey_;
	start_->setEnabled(stage_ == Stage::Idle ? !problem : stage_ == Stage::Running && !here);
	start_->setText(stage_ == Stage::Running && !here ? tr("Switch to this case") : tr("Start"));
	stop_->setEnabled(stage_ != Stage::Idle && stage_ != Stage::Stopping);
	record_->setEnabled(stage_ == Stage::Running && !guiPath().isEmpty());
	record_->setToolTip(guiPath().isEmpty()
		? tr("hitchtracegui was not found: build src/hitchtracegui/ (it is looked for in ../hitchtracegui/ "
		     "next to this program, then on PATH)")
		: tr("Opens hitchtracegui's Record dialog with the game selected"));
	switch (stage_) {
	case Stage::Idle:
		status_->setText(tr("Not running. Pick a case and press Start: the game opens in its own window."));
		break;
	case Stage::Preparing:
		status_->setText(tr("Writing the files this case reads (once; up to a minute)…"));
		break;
	case Stage::Starting:
		status_->setText(tr("Starting <b>%1</b>…").arg(runningTitle_.toHtmlEscaped()));
		break;
	case Stage::Stopping:
		status_->setText(tr("Stopping…"));
		break;
	case Stage::Running: {
		const qint64 s = clock_.elapsed() / 1000;
		QString t = tr("Running <b>%1</b>. The game is process <b>%2</b> (mg_main), running for %3:%4.")
				    .arg(runningTitle_.toHtmlEscaped())
				    .arg(gamePid())
				    .arg(s / 60)
				    .arg(s % 60, 2, 10, QLatin1Char('0'));
		if (plan_.stopAfter) {
			const qint64 left = std::max<qint64>(0, plan_.stopAfter - s);
			t += tr(" Stops by itself in %1 s.").arg(left);
			if (left == 0)
				QTimer::singleShot(0, this, &LabWindow::stop);
		}
		status_->setText(t);
		break;
	}
	}
}

QString LabWindow::guiPath() const
{
	const QString beside =
		QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../hitchtracegui/hitchtracegui"));
	if (QFileInfo(beside).isExecutable())
		return beside;
	return QStandardPaths::findExecutable(QStringLiteral("hitchtracegui"));
}

void LabWindow::recordGame()
{
	const qint64 pid = gamePid();
	if (!pid)
		return;
	if (!QProcess::startDetached(guiPath(), { QStringLiteral("--record"), QString::number(pid) }))
		log(QStringLiteral("lab"), tr("could not start %1").arg(guiPath()));
}

void LabWindow::closeEvent(QCloseEvent *e)
{
	stop();
	QMainWindow::closeEvent(e);
}

}  // namespace ht
