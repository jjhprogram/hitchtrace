// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/main_window.h"

#include "ui/blame_panel.h"
#include "ui/lanes.h"
#include "ui/record_dialog.h"
#include "ui/ribbon.h"
#include "ui/session_strip.h"
#include "ui/stack_panel.h"
#include "ui/theme.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

namespace ht {

static QLabel *heading(const QString &text)
{
	auto *l = new QLabel(text);
	QFont f = l->font();
	f.setBold(true);
	l->setFont(f);
	return l;
}

static QLabel *hint(const QString &text)
{
	auto *l = new QLabel(text);
	l->setWordWrap(true);
	l->setForegroundRole(QPalette::PlaceholderText);
	return l;
}

MainWindow::MainWindow(Session *session, QWidget *parent) : QMainWindow(parent), session_(session)
{
	setWindowTitle(tr("hitchtracegui"));
	setAcceptDrops(true);
	setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);

	/* actions */
	auto *open = new QAction(tr("&Open recording…"), this);
	open->setShortcut(QKeySequence::Open);
	connect(open, &QAction::triggered, this, &MainWindow::openDialog);
	auto *record = new QAction(tr("&Record…"), this);
	record->setShortcut(Qt::CTRL | Qt::Key_R);
	record->setToolTip(tr("Record a running program with hitchtrace (Ctrl+R)"));
	connect(record, &QAction::triggered, this, [this] { recordDialog(); });
	auto *quit = new QAction(tr("&Quit"), this);
	quit->setShortcut(QKeySequence::Quit);
	connect(quit, &QAction::triggered, this, &QWidget::close);
	prev_ = new QAction(tr("← Previous hitch"), this);
	prev_->setShortcuts({ QKeySequence(Qt::ALT | Qt::Key_Left), QKeySequence(Qt::Key_PageUp) });
	prev_->setToolTip(tr("Previous hitch (Alt+Left, Page Up)"));
	connect(prev_, &QAction::triggered, session, &Session::prevFrame);
	next_ = new QAction(tr("Next hitch →"), this);
	next_->setShortcuts({ QKeySequence(Qt::ALT | Qt::Key_Right), QKeySequence(Qt::Key_PageDown) });
	next_->setToolTip(tr("Next hitch (Alt+Right, Page Down)"));
	connect(next_, &QAction::triggered, session, &Session::nextFrame);
	follow_ = new QAction(tr("Light up the path"), this);
	follow_->setCheckable(true);
	follow_->setChecked(true);
	follow_->setToolTip(tr("Dim everything in the thread lanes that is not on the path being followed"));
	connect(follow_, &QAction::toggled, session, &Session::setFollow);
	auto *back = new QAction(tr("Back to the critical path"), this);
	back->setShortcut(Qt::Key_Escape);
	connect(back, &QAction::triggered, this, [this] {
		if (session_->wait())
			session_->followWait(nullptr);
		else
			session_->clearRange();
	});
	addAction(back);

	/* toolbar */
	auto *tb = addToolBar(tr("Navigation"));
	tb->setObjectName(QStringLiteral("navigation"));
	tb->setMovable(false);
	tb->addAction(record);
	tb->addAction(open);
	tb->addSeparator();
	tb->addWidget(new QLabel(tr(" Recording ")));
	runs_ = new QComboBox;
	runs_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	runs_->setToolTip(tr("The open recordings. Open more with File › Open, or drop hitch.jsonl files here."));
	connect(runs_, &QComboBox::activated, session, &Session::setRun);
	tb->addWidget(runs_);
	tb->addSeparator();
	tb->addAction(prev_);
	tb->addAction(next_);
	tb->addSeparator();
	tb->addAction(follow_);

	/* central: the frame */
	auto *central = new QWidget;
	auto *v = new QVBoxLayout(central);
	v->setContentsMargins(10, 8, 10, 8);
	v->setSpacing(6);
	auto *head = new QHBoxLayout;
	frameTitle_ = new QLabel;
	QFont big = frameTitle_->font();
	big.setPointSizeF(big.pointSizeF() * 1.45);
	big.setBold(true);
	frameTitle_->setFont(big);
	frameOver_ = new QLabel;
	frameOver_->setForegroundRole(QPalette::PlaceholderText);
	head->addWidget(frameTitle_);
	head->addSpacing(10);
	head->addWidget(frameOver_, 1, Qt::AlignBaseline);
	v->addLayout(head);
	v->addWidget(heading(tr("Critical path")));
	v->addWidget(hint(tr("What decided when this frame finished, moment by moment: the presenting thread, and "
			     "whenever it waited, whoever it was waiting for. The parts add up to the frame.")));
	v->addWidget(new Ribbon(session));
	v->addSpacing(6);
	v->addWidget(heading(tr("Threads")));
	v->addWidget(hint(tr("Grey: running. Colour: off the CPU, by what it waited for. The outlined path follows "
			     "the blame across threads. Click any wait to follow its own chain.")));
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(new Lanes(session));
	v->addWidget(scroll, 1);
	waitBar_ = new QWidget;
	auto *wb = new QHBoxLayout(waitBar_);
	wb->setContentsMargins(0, 0, 0, 0);
	waitNote_ = new QLabel;
	auto *backBtn = new QPushButton(tr("Back to the critical path"));
	connect(backBtn, &QPushButton::clicked, this, [this] { session_->followWait(nullptr); });
	wb->addWidget(waitNote_, 1);
	wb->addWidget(backBtn);
	v->addWidget(waitBar_);
	setCentralWidget(central);

	/* docks */
	auto *stripBox = new QWidget;
	auto *sv = new QVBoxLayout(stripBox);
	sv->setContentsMargins(8, 4, 8, 6);
	sv->setSpacing(4);
	sv->addWidget(hint(tr("Every hitch, split by what its critical path was doing. Click a bar to open that "
			      "frame; drag to select a range for the blame tree; right-click or Esc clears it. "
			      "Only over-budget frames are recorded.")));
	sv->addWidget(new SessionStrip(session), 1);
	legend_ = new QLabel;
	legend_->setWordWrap(true);
	legend_->setTextFormat(Qt::RichText);
	sv->addWidget(legend_);
	stripDock_ = new QDockWidget(tr("Session"), this);
	stripDock_->setObjectName(QStringLiteral("session"));
	stripDock_->setWidget(stripBox);
	addDockWidget(Qt::TopDockWidgetArea, stripDock_);

	blameDock_ = new QDockWidget(tr("Blame tree"), this);
	blameDock_->setObjectName(QStringLiteral("blame"));
	blameDock_->setWidget(new BlamePanel(session));
	addDockWidget(Qt::RightDockWidgetArea, blameDock_);
	stackDock_ = new QDockWidget(tr("Stacks"), this);
	stackDock_->setObjectName(QStringLiteral("stacks"));
	stackDock_->setWidget(new StackPanel(session));
	addDockWidget(Qt::RightDockWidgetArea, stackDock_);
	splitDockWidget(blameDock_, stackDock_, Qt::Vertical);
	resizeDocks({ blameDock_ }, { 520 }, Qt::Horizontal);
	resizeDocks({ stripDock_ }, { 230 }, Qt::Vertical);

	/* menus */
	auto *file = menuBar()->addMenu(tr("&File"));
	file->addAction(record);
	file->addAction(open);
	file->addSeparator();
	file->addAction(quit);
	auto *nav = menuBar()->addMenu(tr("&Go"));
	nav->addAction(prev_);
	nav->addAction(next_);
	nav->addAction(back);
	auto *view = menuBar()->addMenu(tr("&View"));
	view->addAction(stripDock_->toggleViewAction());
	view->addAction(blameDock_->toggleViewAction());
	view->addAction(stackDock_->toggleViewAction());
	view->addAction(follow_);
	view->addSeparator();
	auto *reset = view->addAction(tr("Reset layout"));
	connect(reset, &QAction::triggered, this, [this] { restoreState(defaultState_); });

	status_ = new QLabel;
	statusBar()->addWidget(status_, 1);

	defaultState_ = saveState();
	QSettings settings;
	restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
	restoreState(settings.value(QStringLiteral("state")).toByteArray());
	if (!settings.contains(QStringLiteral("geometry")))
		resize(1500, 950);

	connect(session, &Session::changed, this, &MainWindow::refresh);
	connect(session, &Session::runsChanged, this, &MainWindow::refreshRuns);
	refreshLegend();
	refresh();
}

int MainWindow::openFiles(const QStringList &paths)
{
	QStringList errors;
	int loaded = 0;
	for (const auto &path : paths) {
		QString err;
		auto run = loadRun(path, &err);
		if (!run) {
			errors << err;
			continue;
		}
		session_->addRun(std::move(run));
		loaded++;
	}
	if (!errors.isEmpty())
		QMessageBox::warning(this, tr("Some recordings did not open"), errors.join(QLatin1Char('\n')));
	return loaded;
}

bool MainWindow::showFrameId(std::uint64_t id)
{
	const Run *r = session_->run();
	if (!r)
		return false;
	for (int i = 0; i < int(r->frames.size()); i++)
		if (r->frames[i].id == id) {
			session_->setFrame(i);
			return true;
		}
	return false;
}

void MainWindow::openDialog()
{
	const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open hitchtrace recordings"), QString(),
								tr("hitchtrace output (*.jsonl);;All files (*)"));
	if (!paths.isEmpty())
		openFiles(paths);
}

void MainWindow::recordDialog(int pid)
{
	auto *d = new RecordDialog(this);
	d->setAttribute(Qt::WA_DeleteOnClose);
	if (pid)
		d->selectPid(pid);
	connect(d, &RecordDialog::recorded, this, [this](const QString &path) { openFiles({ path }); });
	d->open();
}

void MainWindow::refreshRuns()
{
	runs_->clear();
	for (const auto &r : session_->runs())
		runs_->addItem(r->title, r->path);
	runs_->setCurrentIndex(session_->runIndex());
}

void MainWindow::refreshLegend()
{
	const Theme t = Theme::current();
	QString html;
	for (int c = 0; c < int(Cat::Open); c++)
		html += QStringLiteral("<span style=\"color:%1\">■</span>&nbsp;%2&nbsp;&nbsp; ")
				.arg(t.fill(Cat(c)).name(), catLabel(Cat(c)));
	html += QStringLiteral("▨&nbsp;another process (not traced)&nbsp;&nbsp; □&nbsp;the path being followed");
	legend_->setText(html);
}

void MainWindow::refresh()
{
	const Run *r = session_->run();
	const Frame *f = session_->frame();
	if (runs_->currentIndex() != session_->runIndex())
		runs_->setCurrentIndex(session_->runIndex());
	prev_->setEnabled(r && session_->frameIndex() > 0);
	next_->setEnabled(r && session_->frameIndex() + 1 < int(r->frames.size()));
	if (!r || !f) {
		frameTitle_->setText(tr("No recording open"));
		frameOver_->setText(tr("Record a running program (File › Record), or open a hitch.jsonl that hitchtrace "
				       "wrote (File › Open, or drop it here)."));
		status_->clear();
		waitBar_->hide();
		return;
	}
	frameTitle_->setText(tr("Frame %1: %2").arg(f->id).arg(fmtMs(f->duration)));
	frameOver_->setText(tr("%1 over the %2 budget · hitch %3 of %4")
				    .arg(fmtMs(f->duration - r->budget), fmtMs(r->budget))
				    .arg(session_->frameIndex() + 1)
				    .arg(r->frames.size()));
	status_->setText(tr("%1 · process %2 · %3 hitches over %4 s · %5")
				 .arg(r->title)
				 .arg(r->pid)
				 .arg(r->frames.size())
				 .arg(r->span / 1e9, 0, 'f', 0)
				 .arg(r->path));
	if (const Event *e = session_->wait()) {
		waitNote_->setText(tr("Following one wait: %1, %2 from %3 ms. The blame tree shows its chain.")
					   .arg(threadName(*r, *f, e->tid), fmtMs(e->wake - e->out))
					   .arg(std::max<std::int64_t>(0, e->out) / 1e6, 0, 'f', 1));
		waitBar_->show();
	} else {
		waitBar_->hide();
	}
	setWindowTitle(tr("%1 – hitchtracegui").arg(r->title));
}

void MainWindow::closeEvent(QCloseEvent *e)
{
	QSettings settings;
	settings.setValue(QStringLiteral("geometry"), saveGeometry());
	settings.setValue(QStringLiteral("state"), saveState());
	QMainWindow::closeEvent(e);
}

void MainWindow::changeEvent(QEvent *e)
{
	if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange)
		refreshLegend();
	QMainWindow::changeEvent(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
	if (e->mimeData()->hasUrls())
		e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
	QStringList paths;
	for (const auto &u : e->mimeData()->urls())
		if (u.isLocalFile())
			paths << u.toLocalFile();
	if (!paths.isEmpty())
		openFiles(paths);
}

}  // namespace ht
