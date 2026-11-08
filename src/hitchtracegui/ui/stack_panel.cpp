// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/stack_panel.h"

#include "ui/theme.h"

#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

namespace ht {

namespace {

struct Row {
	QString thread, where;
	StackOf stack;
	std::int64_t ns = 0;
	QSet<const Event *> waits;
	int depth = 0;		// 0: the thread the path starts on
};

QString sourceNote(StackOf::Source s)
{
	switch (s) {
	case StackOf::Exact: return {};
	case StackOf::Group: return QStringLiteral("the stack of the longest wait in its stall group");
	case StackOf::Largest: return QStringLiteral("the stack of this thread's longest wait, which blocked in the same place");
	case StackOf::TopOnly: return QStringLiteral("only the function it blocked in was recorded");
	}
	return {};
}

}  // namespace

StackPanel::StackPanel(Session *session, QWidget *parent) : QWidget(parent), session_(session)
{
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(8, 6, 8, 6);
	v->setSpacing(4);
	what_ = new QLabel;
	what_->setWordWrap(true);
	v->addWidget(what_);
	tree_ = new QTreeWidget;
	tree_->setColumnCount(3);
	tree_->setHeaderLabels({ tr("Where it was blocked"), tr("ms"), tr("Waits") });
	tree_->header()->setStretchLastSection(false);
	tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	tree_->setUniformRowHeights(true);
	v->addWidget(tree_, 1);
	note_ = new QLabel;
	note_->setWordWrap(true);
	note_->setForegroundRole(QPalette::PlaceholderText);
	v->addWidget(note_);
	connect(session, &Session::changed, this, &StackPanel::refresh);
	refresh();
}

void StackPanel::refresh()
{
	const Run *run = session_->run();
	const Frame *frame = session_->frame();
	if (!run || !frame) {
		what_->clear();
		tree_->clear();
		note_->clear();
		shownFor_.clear();
		return;
	}

	// rebuild only when the focus changed (hover alone does not change it)
	const QString key = QStringLiteral("%1|%2|%3|%4|%5")
				    .arg(quintptr(run)).arg(session_->treeGeneration()).arg(session_->zoom().join(QLatin1Char('>')))
				    .arg(session_->selectedSegment()).arg(quintptr(session_->wait()));
	if (key == shownFor_)
		return;
	shownFor_ = key;
	// what is in focus: segments, each with the frame it belongs to
	std::vector<std::pair<const Frame *, Segment>> focus;
	QString what;
	const auto scope = session_->scope();
	if (scope == Session::Scope::Wait && session_->wait()) {
		for (auto &s : explainWait(*run, *frame, *session_->wait()))
			focus.push_back({ frame, std::move(s) });
		what = tr("the wait being followed (%1)").arg(threadName(*run, *frame, session_->wait()->tid));
	} else if (!session_->selectedSegment().isEmpty() && scope == Session::Scope::Frame) {
		// the picked part of the ribbon: the pieces of the critical path it joined
		const auto &key = session_->selectedSegment();
		for (const auto &m : merged(criticalPath(*run, *frame)))
			if (Session::segmentKey(m) == key)
				for (const auto &s : criticalPath(*run, *frame))
					if (s.tid == m.tid && s.a >= m.a && s.b <= m.b)
						focus.push_back({ frame, s });
		what = tr("the part of the critical path picked on the ribbon");
	} else {
		std::vector<const Frame *> frames;
		if (scope == Session::Scope::Selection && session_->hasRange()) {
			frames = session_->rangeFrames();
			what = tr("the critical paths of the %1 selected hitches").arg(frames.size());
		} else if (scope == Session::Scope::All) {
			for (const auto &f : run->frames)
				frames.push_back(&f);
			what = tr("the critical paths of all %1 hitches").arg(frames.size());
		} else {
			frames = { frame };
			what = tr("the critical path of frame %1").arg(frame->id);
		}
		for (const Frame *f : frames)
			for (const auto &s : criticalPath(*run, *f))
				focus.push_back({ f, s });
	}
	// narrowed to the box zoomed into in the blame tree
	QStringList zoom = session_->zoom();
	zoom.removeIf([](const QString &k) { return k.startsWith(QLatin1String("kern")) || k == QLatin1String("code"); });
	if (!zoom.isEmpty()) {
		std::erase_if(focus, [&](const auto &fs) {
			const auto &p = fs.second.path;
			if (p.size() < size_t(zoom.size()))
				return true;
			for (int i = 0; i < zoom.size(); i++)
				if (p[i].key != zoom[i])
					return true;
			return false;
		});
		what += tr(", under “%1”").arg(session_->scopeTree() ? [&] {
			const BlameNode *n = session_->scopeTree();
			for (const auto &k : session_->zoom())
				if (auto it = n->byKey.find(k); it != n->byKey.end())
					n = *it;
			return n->label;
		}() : QString());
	}

	what_->setText(tr("Where the threads were blocked on %1. Each row cost the path the time shown.").arg(what));

	// one row per thread and blocking stack
	std::map<QString, Row> rows;
	std::int64_t total = 0, running = 0;
	bool approximate = false, anyUser = false;
	for (const auto &[f, s] : focus) {
		const std::int64_t d = s.b - s.a;
		total += d;
		bool blocked = false;
		for (size_t depth = 0; depth < s.path.size(); depth++) {
			const Node &n = s.path[depth];
			if (!n.ev)
				continue;
			blocked = true;
			const StackOf st = stackOf(*run, *f, *n.ev);
			const QString thread = threadName(*run, *f, n.ev->tid);
			const QString rk = thread + QLatin1Char('\x1f') + st.kernel.join(QLatin1Char('\x1e')) +
					   QLatin1Char('\x1f') + st.user.join(QLatin1Char('\x1e'));
			Row &r = rows[rk];
			if (r.waits.isEmpty()) {
				r.thread = thread;
				r.where = n.ev->blockedIn.isEmpty() ? causeWord(n.ev->cause) : n.ev->blockedIn;
				r.stack = st;
				r.depth = int(depth);
			}
			r.ns += d;
			r.waits.insert(n.ev);
			approximate |= st.source != StackOf::Exact;
			anyUser |= !st.user.isEmpty();
		}
		if (!blocked)
			running += d;
	}
	std::vector<const Row *> order;
	for (const auto &[k, r] : rows)
		order.push_back(&r);
	std::sort(order.begin(), order.end(), [](const Row *a, const Row *b) {
		return a->ns != b->ns ? a->ns > b->ns : a->depth < b->depth;
	});

	tree_->clear();
	const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	const QString share = QStringLiteral("%1%");
	for (const Row *r : order) {
		auto *it = new QTreeWidgetItem(tree_);
		it->setText(0, tr("%1 · blocked in %2").arg(r->thread, r->where));
		it->setText(1, QString::number(r->ns / 1e6, 'f', 1));
		it->setText(2, QString::number(r->waits.size()));
		it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
		it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
		it->setToolTip(0, tr("%1 of the time in focus (%2)").arg(share.arg(total ? 100.0 * r->ns / total : 0, 0, 'f', 0),
									     r->depth ? tr("a thread waited for along the chain")
										      : tr("the thread the path starts on")));
		auto frame = [&](const QString &text, bool dim) {
			auto *c = new QTreeWidgetItem(it);
			c->setText(0, text);
			c->setFont(0, mono);
			if (dim)
				c->setForeground(0, palette().brush(QPalette::PlaceholderText));
		};
		for (const QString &k : r->stack.kernel)
			frame(k, false);
		if (!r->stack.user.isEmpty()) {
			frame(tr("— in the game's own code —"), true);
			for (const QString &u : r->stack.user)
				frame(u, false);
		}
		if (const QString n = sourceNote(r->stack.source); !n.isEmpty())
			frame(tr("(%1)").arg(n), true);
	}
	if (tree_->topLevelItemCount())
		tree_->topLevelItem(0)->setExpanded(true);

	QStringList notes;
	if (running)
		notes << tr("%1 of the time in focus the path was running, not blocked: stacks are taken when a thread "
			    "stops running, so there is none for that.").arg(fmtMs(running));
	if (approximate)
		notes << tr("This recording does not have every wait's own stack; rows marked in brackets show the "
			    "closest one it has. Recordings made with the current hitchtrace have them all.");
	if (!anyUser)
		notes << tr("The game's own frames appear only for the presenting thread's waits, and only when the "
			    "recording was made with user stacks (Record › “Also record where in the game's own "
			    "code…”, or hitchtrace -U).");
	note_->setText(notes.join(QLatin1Char(' ')));
}

}  // namespace ht
