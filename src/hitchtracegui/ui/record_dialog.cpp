// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/record_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <unistd.h>

namespace ht {

namespace {

const QString LayerName = QStringLiteral("libVkLayer_hitchtrace.so");

QString readFile(const QString &path, qint64 max = 1 << 20)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QString::fromLocal8Bit(f.readAll().left(max));
}

// The hitchtrace layer as the process mapped it, "" if it has not, or a null
// string if its memory map could not be read (another user's process).
QString mappedLayer(int pid)
{
	QFile f(QStringLiteral("/proc/%1/maps").arg(pid));
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	// /proc files report a size of 0, so read to the end rather than test atEnd()
	for (const QByteArray &line : f.readAll().split('\n')) {
		if (!line.contains("libVkLayer_hitchtrace"))
			continue;
		const int slash = line.indexOf('/');
		if (slash >= 0)
			return QString::fromLocal8Bit(line.mid(slash).trimmed());
	}
	return QStringLiteral("");
}

QString shellQuote(const QString &s)
{
	static const QString safe = QStringLiteral("-_./=:+,%@");
	bool plain = !s.isEmpty();
	for (QChar c : s)
		plain &= c.isLetterOrNumber() || safe.contains(c);
	if (plain)
		return s;
	QString q = s;
	q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
	return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString defaultTracer()
{
	// build/hitchtracegui/hitchtracegui next to build/hitchtrace, else one on PATH
	const QString beside = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../hitchtrace"));
	if (QFileInfo(beside).isExecutable())
		return beside;
	return QStandardPaths::findExecutable(QStringLiteral("hitchtrace"));
}

int countRecords(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return 0;
	int n = 0;
	while (!f.atEnd())
		n += f.readLine().contains("\"frame_id\"");
	return n;
}

}  // namespace

RecordDialog::RecordDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(tr("Record"));
	resize(860, 720);
	auto *v = new QVBoxLayout(this);
	pages_ = new QStackedWidget;
	pages_->addWidget(buildSetup());
	pages_->addWidget(buildRunning());
	v->addWidget(pages_);
	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, &RecordDialog::tick);

	QSettings s;
	tracer_->setText(s.value(QStringLiteral("record/tracer"), defaultTracer()).toString());
	budgetMode_->setCurrentIndex(s.value(QStringLiteral("record/budgetMode"), 0).toInt());
	budgetMs_->setValue(s.value(QStringLiteral("record/budgetMs"), 20.8).toDouble());
	seconds_->setValue(s.value(QStringLiteral("record/seconds"), 0).toInt());
	userStacks_->setChecked(s.value(QStringLiteral("record/userStacks"), false).toBool());
	checkTracer();
	refreshPrograms();
}

RecordDialog::~RecordDialog()
{
	if (recording()) {
		// hitchtrace --stop-on-eof finishes once its input closes
		proc_->closeWriteChannel();
		proc_->waitForFinished(8000);
	}
}

QWidget *RecordDialog::buildSetup()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);

	auto *progBox = new QGroupBox(tr("Program to record"));
	auto *pv = new QVBoxLayout(progBox);
	auto *hint = new QLabel(tr("A program shows up as ready once it has loaded the hitchtrace Vulkan layer: start it "
				   "with <code>HITCHTRACE=1</code> in its environment (for a Steam game, set its launch "
				   "options to <code>HITCHTRACE=1 %command%</code>). The layer must be installed "
				   "(<code>make layer layer-install</code>)."));
	hint->setWordWrap(true);
	hint->setTextFormat(Qt::RichText);
	pv->addWidget(hint);
	auto *row = new QHBoxLayout;
	filter_ = new QLineEdit;
	filter_->setPlaceholderText(tr("Filter by name or process id"));
	filter_->setClearButtonEnabled(true);
	onlyLayer_ = new QCheckBox(tr("Only programs that are ready"));
	onlyLayer_->setChecked(true);
	auto *refresh = new QPushButton(tr("Refresh"));
	row->addWidget(filter_, 1);
	row->addWidget(onlyLayer_);
	row->addWidget(refresh);
	pv->addLayout(row);
	programs_ = new QTreeWidget;
	programs_->setColumnCount(4);
	programs_->setHeaderLabels({ tr("PID"), tr("Program"), tr("Layer"), tr("Command line") });
	programs_->setRootIsDecorated(false);
	programs_->setUniformRowHeights(true);
	programs_->setAlternatingRowColors(true);
	programs_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
	programs_->header()->setStretchLastSection(true);
	pv->addWidget(programs_, 1);
	v->addWidget(progBox, 1);
	connect(refresh, &QPushButton::clicked, this, &RecordDialog::refreshPrograms);
	connect(filter_, &QLineEdit::textChanged, this, &RecordDialog::refreshPrograms);
	connect(onlyLayer_, &QCheckBox::toggled, this, &RecordDialog::refreshPrograms);
	connect(programs_, &QTreeWidget::currentItemChanged, this, [this] {
		if (const Program *p = selected()) {
			// a fresh file name for each program picked, unless the user typed one
			if (output_->property("auto").toBool() || output_->text().isEmpty()) {
				output_->setText(defaultOutput(*p));
				output_->setProperty("auto", true);
			}
		}
		updateState();
	});

	auto *setBox = new QGroupBox(tr("Recording"));
	auto *form = new QFormLayout(setBox);
	auto *budgetRow = new QHBoxLayout;
	budgetMode_ = new QComboBox;
	budgetMode_->addItems({ tr("Fixed"), tr("Follow the game's frame rate") });
	budgetMode_->setToolTip(tr("Follow: the budget is 1.5 × the median of the last 19 frames, for a game with no "
				   "frame cap"));
	budgetMs_ = new QDoubleSpinBox;
	budgetMs_->setRange(1.0, 1000.0);
	budgetMs_->setDecimals(1);
	budgetMs_->setSuffix(tr(" ms"));
	budgetMs_->setToolTip(tr("Only frames longer than this are recorded. 16.7 ms is exactly 60 fps; 20.8 ms, "
				  "25% over that, leaves out frames that are only just late."));
	budgetRow->addWidget(budgetMode_);
	budgetRow->addWidget(budgetMs_);
	budgetRow->addStretch(1);
	form->addRow(tr("Frame budget"), budgetRow);
	seconds_ = new QSpinBox;
	seconds_->setRange(0, 24 * 3600);
	seconds_->setSuffix(tr(" s"));
	seconds_->setSpecialValueText(tr("until I stop it"));
	form->addRow(tr("Record for"), seconds_);
	userStacks_ = new QCheckBox(tr("Also record where in the game's own code the main thread waited (-U)"));
	form->addRow(QString(), userStacks_);
	auto *outRow = new QHBoxLayout;
	output_ = new QLineEdit;
	auto *browseOut = new QPushButton(tr("Choose…"));
	outRow->addWidget(output_, 1);
	outRow->addWidget(browseOut);
	form->addRow(tr("Save to"), outRow);
	auto *trRow = new QHBoxLayout;
	tracer_ = new QLineEdit;
	auto *browseTr = new QPushButton(tr("Choose…"));
	trRow->addWidget(tracer_, 1);
	trRow->addWidget(browseTr);
	form->addRow(tr("hitchtrace"), trRow);
	rootNote_ = new QLabel;
	rootNote_->setWordWrap(true);
	form->addRow(QString(), rootNote_);
	v->addWidget(setBox);

	auto *cmdLabel = new QLabel(tr("Will run:"));
	v->addWidget(cmdLabel);
	command_ = new QPlainTextEdit;
	command_->setReadOnly(true);
	command_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	command_->setFixedHeight(command_->fontMetrics().lineSpacing() * 3 + 12);
	v->addWidget(command_);

	auto *buttons = new QHBoxLayout;
	problem_ = new QLabel;
	problem_->setWordWrap(true);
	problem_->setTextFormat(Qt::RichText);
	record_ = new QPushButton(tr("Record"));
	record_->setDefault(true);
	auto *close = new QPushButton(tr("Close"));
	buttons->addWidget(problem_, 1);
	buttons->addWidget(record_);
	buttons->addWidget(close);
	v->addLayout(buttons);

	connect(record_, &QPushButton::clicked, this, &RecordDialog::startRecording);
	connect(close, &QPushButton::clicked, this, &RecordDialog::reject);
	connect(budgetMode_, &QComboBox::currentIndexChanged, this, &RecordDialog::updateState);
	connect(budgetMs_, &QDoubleSpinBox::valueChanged, this, &RecordDialog::updateState);
	connect(seconds_, &QSpinBox::valueChanged, this, &RecordDialog::updateState);
	connect(userStacks_, &QCheckBox::toggled, this, &RecordDialog::updateState);
	connect(output_, &QLineEdit::textEdited, this, [this] {
		output_->setProperty("auto", false);
		updateState();
	});
	connect(tracer_, &QLineEdit::editingFinished, this, [this] {
		checkTracer();
		updateState();
	});
	connect(browseOut, &QPushButton::clicked, this, [this] {
		const QString p = QFileDialog::getSaveFileName(this, tr("Save the recording as"), output_->text(),
							       tr("hitchtrace recording (*.jsonl)"), nullptr,
							       QFileDialog::DontConfirmOverwrite);
		if (!p.isEmpty()) {
			output_->setText(p);
			output_->setProperty("auto", false);
			updateState();
		}
	});
	connect(browseTr, &QPushButton::clicked, this, [this] {
		const QString p = QFileDialog::getOpenFileName(this, tr("Where is hitchtrace?"), tracer_->text());
		if (!p.isEmpty()) {
			tracer_->setText(p);
			checkTracer();
			updateState();
		}
	});
	return w;
}

QWidget *RecordDialog::buildRunning()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	runTitle_ = new QLabel;
	QFont f = runTitle_->font();
	f.setBold(true);
	f.setPointSizeF(f.pointSizeF() * 1.25);
	runTitle_->setFont(f);
	runStatus_ = new QLabel;
	runStatus_->setWordWrap(true);
	v->addWidget(runTitle_);
	v->addWidget(runStatus_);
	v->addWidget(new QLabel(tr("hitchtrace's output:")));
	log_ = new QPlainTextEdit;
	log_->setReadOnly(true);
	log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	log_->setMaximumBlockCount(3000);
	log_->setLineWrapMode(QPlainTextEdit::NoWrap);
	v->addWidget(log_, 1);
	auto *buttons = new QHBoxLayout;
	buttons->addStretch(1);
	back_ = new QPushButton(tr("Back"));
	stop_ = new QPushButton(tr("Stop and open"));
	stop_->setDefault(true);
	buttons->addWidget(back_);
	buttons->addWidget(stop_);
	v->addLayout(buttons);
	connect(stop_, &QPushButton::clicked, this, &RecordDialog::stopRecording);
	connect(back_, &QPushButton::clicked, this, [this] {
		pages_->setCurrentIndex(0);
		refreshPrograms();
		// the last file name is taken now; offer a fresh one
		if (const Program *p = selected(); p && output_->property("auto").toBool())
			output_->setText(defaultOutput(*p));
		updateState();
	});
	return w;
}

void RecordDialog::checkTracer()
{
	const QString path = tracer_->text();
	if (path == rootFor_)
		return;
	rootFor_ = path;
	root_ = Root::None;
	if (!QFileInfo(path).isExecutable())
		return;
	// a sudo rule that needs no password, else the system's own prompt
	QProcess p;
	p.start(QStringLiteral("sudo"), { QStringLiteral("-n"), path, QStringLiteral("--version") });
	if (p.waitForFinished(3000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
		root_ = Root::Sudo;
	else if (!QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty())
		root_ = Root::Pkexec;
}

void RecordDialog::refreshPrograms()
{
	const Program *cur = selected();
	const int keep = cur ? cur->pid : 0;
	list_.clear();
	const uid_t me = ::getuid();
	const QStringList dirs = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto &d : dirs) {
		bool ok = false;
		const int pid = d.toInt(&ok);
		if (!ok || pid == QCoreApplication::applicationPid())
			continue;
		Program p;
		p.pid = pid;
		QString cmd = readFile(QStringLiteral("/proc/%1/cmdline").arg(pid), 4096);
		if (cmd.isEmpty())
			continue;	// a kernel thread
		p.cmdline = cmd.replace(QChar(0), QLatin1Char(' ')).trimmed();
		p.name = readFile(QStringLiteral("/proc/%1/comm").arg(pid)).trimmed();
		p.layer = QFileInfo(QStringLiteral("/proc/%1").arg(pid)).ownerId() == me ? mappedLayer(pid) : QString();
		list_.push_back(std::move(p));
	}
	std::sort(list_.begin(), list_.end(), [](const Program &a, const Program &b) {
		if (a.layer.isEmpty() != b.layer.isEmpty())
			return !a.layer.isEmpty();
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});

	const QString f = filter_->text().trimmed();
	programs_->clear();
	QTreeWidgetItem *again = nullptr;
	for (size_t i = 0; i < list_.size(); i++) {
		const Program &p = list_[i];
		if (onlyLayer_->isChecked() && p.layer.isEmpty())
			continue;
		if (!f.isEmpty() && !p.name.contains(f, Qt::CaseInsensitive) && !QString::number(p.pid).startsWith(f)
		    && !p.cmdline.contains(f, Qt::CaseInsensitive))
			continue;
		auto *it = new QTreeWidgetItem(programs_);
		it->setText(0, QString::number(p.pid));
		it->setText(1, p.name);
		it->setText(2, !p.layer.isEmpty() ? tr("ready") : p.layer.isNull() ? tr("can't tell") : QString());
		it->setToolTip(2, !p.layer.isEmpty() ? p.layer
				: p.layer.isNull() ? tr("Another user's process: its memory map cannot be read")
						   : tr("The hitchtrace layer is not loaded in this process"));
		it->setText(3, p.cmdline);
		it->setToolTip(3, p.cmdline);
		it->setData(0, Qt::UserRole, int(i));
		if (p.pid == keep)
			again = it;
	}
	for (int c = 0; c < 3; c++)
		programs_->resizeColumnToContents(c);
	if (again)
		programs_->setCurrentItem(again);
	else if (programs_->topLevelItemCount() == 1)
		programs_->setCurrentItem(programs_->topLevelItem(0));
	updateState();
}

const RecordDialog::Program *RecordDialog::selected() const
{
	const QTreeWidgetItem *it = programs_->currentItem();
	if (!it)
		return nullptr;
	const int i = it->data(0, Qt::UserRole).toInt();
	return i >= 0 && i < int(list_.size()) ? &list_[i] : nullptr;
}

bool RecordDialog::selectPid(int pid)
{
	for (int i = 0; i < programs_->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = programs_->topLevelItem(i);
		if (it->text(0).toInt() == pid) {
			programs_->setCurrentItem(it);
			return true;
		}
	}
	return false;
}

QString RecordDialog::defaultOutput(const Program &p) const
{
	QSettings s;
	const QString dir = s.value(QStringLiteral("record/dir"), QDir::homePath() + QStringLiteral("/hitchtrace")).toString();
	QString name = p.name;
	name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
	return QStringLiteral("%1/%2-%3.jsonl").arg(dir, name, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

QStringList RecordDialog::tracerArgs() const
{
	const Program *p = selected();
	if (!p)
		return {};
	// attach by the path the process mapped; inside a container that path is
	// only valid in its mount namespace, which /proc/PID/root reaches
	const QString layer = QFileInfo::exists(p->layer) ? p->layer : QStringLiteral("/proc/%1/root%2").arg(p->pid).arg(p->layer);
	QStringList a = { QStringLiteral("-p"), QString::number(p->pid), QStringLiteral("-x"), layer, QStringLiteral("-o"),
			  QFileInfo(output_->text()).absoluteFilePath(), QStringLiteral("--stop-on-eof") };
	if (budgetMode_->currentIndex() == 1)
		a << QStringLiteral("--budget-mode=median");
	else
		a << QStringLiteral("-b") << QString::number(qRound(budgetMs_->value() * 1000));
	if (seconds_->value() > 0)
		a << QStringLiteral("-d") << QString::number(seconds_->value());
	if (userStacks_->isChecked())
		a << QStringLiteral("-U");
	return a;
}

void RecordDialog::updateState()
{
	const Program *p = selected();
	budgetMs_->setEnabled(budgetMode_->currentIndex() == 0);
	switch (root_) {
	case Root::Sudo: rootNote_->setText(tr("Runs as root through sudo, without a password.")); break;
	case Root::Pkexec: rootNote_->setText(tr("Runs as root through pkexec: your system will ask for your password.")); break;
	case Root::None: rootNote_->clear(); break;
	}

	QString problem;
	const QString out = output_->text().trimmed();
	if (!QFileInfo(tracer_->text()).isExecutable())
		problem = tr("hitchtrace was not found: build it with <code>make</code>, or choose where it is.");
	else if (root_ == Root::None)
		problem = tr("Cannot run hitchtrace as root: there is no sudo rule that needs no password, and no pkexec.");
	else if (!p)
		problem = list_.empty() || programs_->topLevelItemCount() == 0
			? tr("No program is ready. Start one with <code>HITCHTRACE=1</code>, then press Refresh.")
			: tr("Pick the program to record.");
	else if (p->layer.isEmpty())
		problem = tr("%1 has not loaded the hitchtrace layer. Start it again with <code>HITCHTRACE=1</code>.")
				  .arg(p->name.toHtmlEscaped());
	else if (out.isEmpty())
		problem = tr("Choose where to save the recording.");
	else if (QFileInfo(out).exists() && QFileInfo(out).size() > 0)
		problem = tr("%1 already exists. hitchtrace adds to a file, and two recordings in one file do not "
			     "read back well: choose a new name.")
				  .arg(QFileInfo(out).fileName().toHtmlEscaped());
	problem_->setText(problem);
	record_->setEnabled(problem.isEmpty());

	QStringList cmd;
	if (root_ == Root::Sudo)
		cmd << QStringLiteral("sudo") << QStringLiteral("-n");
	else if (root_ == Root::Pkexec)
		cmd << QStringLiteral("pkexec");
	cmd << tracer_->text() << tracerArgs();
	QStringList quoted;
	for (const auto &c : cmd)
		quoted << shellQuote(c);
	command_->setPlainText(p ? quoted.join(QLatin1Char(' ')) : QString());
}

bool RecordDialog::recording() const
{
	return proc_ && proc_->state() != QProcess::NotRunning;
}

void RecordDialog::startRecording()
{
	updateState();
	const Program *p = selected();
	if (!record_->isEnabled() || !p)
		return;
	QSettings s;
	s.setValue(QStringLiteral("record/tracer"), tracer_->text());
	s.setValue(QStringLiteral("record/budgetMode"), budgetMode_->currentIndex());
	s.setValue(QStringLiteral("record/budgetMs"), budgetMs_->value());
	s.setValue(QStringLiteral("record/seconds"), seconds_->value());
	s.setValue(QStringLiteral("record/userStacks"), userStacks_->isChecked());

	outPath_ = QFileInfo(output_->text().trimmed()).absoluteFilePath();
	s.setValue(QStringLiteral("record/dir"), QFileInfo(outPath_).absolutePath());
	QDir().mkpath(QFileInfo(outPath_).absolutePath());
	// created here so the file is the user's: hitchtrace, as root, only appends
	QFile touch(outPath_);
	if (!touch.open(QIODevice::WriteOnly | QIODevice::Append)) {
		problem_->setText(tr("Cannot write %1: %2").arg(outPath_.toHtmlEscaped(), touch.errorString()));
		return;
	}
	touch.close();

	const QStringList args = tracerArgs();
	delete proc_;
	proc_ = new QProcess(this);
	if (root_ == Root::Sudo) {
		proc_->setProgram(QStringLiteral("sudo"));
		proc_->setArguments(QStringList{ QStringLiteral("-n"), tracer_->text() } + args);
	} else {
		proc_->setProgram(QStringLiteral("pkexec"));
		proc_->setArguments(QStringList{ tracer_->text() } + args);
	}
	connect(proc_, &QProcess::readyReadStandardOutput, this, &RecordDialog::onOutput);
	connect(proc_, &QProcess::readyReadStandardError, this, &RecordDialog::onOutput);
	connect(proc_, &QProcess::finished, this, &RecordDialog::onFinished);
	connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
		if (e == QProcess::FailedToStart) {
			runStatus_->setText(tr("hitchtrace could not be started: %1").arg(proc_->errorString()));
			stop_->hide();
			back_->show();
			timer_->stop();
		}
	});

	hitches_ = 0;
	stopping_ = false;
	duration_ = seconds_->value();
	outBuf_.clear();
	log_->clear();
	runTitle_->setText(tr("Recording %1 (process %2)").arg(p->name).arg(p->pid));
	runStatus_->setText(root_ == Root::Pkexec ? tr("Waiting for your system to ask for your password…")
						  : tr("Starting hitchtrace…"));
	stop_->show();
	stop_->setEnabled(true);
	back_->hide();
	pages_->setCurrentIndex(1);
	clock_.start();
	timer_->start(500);
	proc_->start();
}

void RecordDialog::onOutput()
{
	if (!proc_)
		return;
	const QString text = QString::fromLocal8Bit(proc_->readAllStandardError() + proc_->readAllStandardOutput());
	outBuf_ += text;
	const int cut = outBuf_.lastIndexOf(QLatin1Char('\n'));
	if (cut < 0)
		return;
	for (const QString &line : outBuf_.left(cut).split(QLatin1Char('\n'))) {
		if (line.startsWith(QLatin1String("hitch frame")))
			hitches_++;
		log_->appendPlainText(line);
	}
	outBuf_ = outBuf_.mid(cut + 1);
}

void RecordDialog::tick()
{
	if (!recording() || stopping_)
		return;
	const qint64 s = clock_.elapsed() / 1000;
	QString t = tr("Recording for %1:%2 · %3 hitches so far")
			    .arg(s / 60)
			    .arg(s % 60, 2, 10, QLatin1Char('0'))
			    .arg(hitches_);
	t += duration_ ? tr(" · stops by itself after %1 s").arg(duration_)
		       : tr(" · runs until you stop it or the program exits");
	runStatus_->setText(t);
}

void RecordDialog::stopRecording()
{
	if (!recording() || stopping_)
		return;
	stopping_ = true;
	stop_->setEnabled(false);
	runStatus_->setText(tr("Stopping: hitchtrace is writing out the last frames…"));
	proc_->closeWriteChannel();
	QTimer::singleShot(10000, this, [this] {
		if (recording())
			proc_->terminate();
	});
}

void RecordDialog::onFinished(int code, QProcess::ExitStatus status)
{
	timer_->stop();
	onOutput();
	if (!outBuf_.isEmpty()) {
		log_->appendPlainText(outBuf_);
		outBuf_.clear();
	}
	const int records = countRecords(outPath_);
	const bool asked = stopping_ || property("closeWhenDone").toBool();
	if (records > 0) {
		emit recorded(outPath_);
		QDialog::accept();
		return;
	}
	stop_->hide();
	back_->show();
	if (status != QProcess::NormalExit || (code != 0 && !asked)) {
		runStatus_->setText(code == 126 || code == 127
			? tr("hitchtrace did not start: permission was not given.")
			: tr("hitchtrace stopped with an error (exit code %1). Its output is below.").arg(code));
	} else {
		runStatus_->setText(tr("No frame went over the budget, so the recording has no hitches to show. "
				       "Try a smaller budget, or record for longer."));
	}
	if (property("closeWhenDone").toBool())
		QDialog::reject();
}

void RecordDialog::reject()
{
	if (recording()) {
		// finish the recording first; the window closes when hitchtrace is done
		setProperty("closeWhenDone", true);
		stopRecording();
		return;
	}
	QDialog::reject();
}

}  // namespace ht
