// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/blame_panel.h"

#include "ui/blame_view.h"
#include "ui/theme.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

namespace ht {

BlamePanel::BlamePanel(Session *session, QWidget *parent) : QWidget(parent), session_(session)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(6);

	auto *row = new QHBoxLayout;
	row->setSpacing(4);
	auto *what = new QLabel(tr("Covers"));
	row->addWidget(what);
	group_ = new QButtonGroup(this);
	auto button = [&](const QString &text, Session::Scope scope) {
		auto *b = new QToolButton;
		b->setText(text);
		b->setCheckable(true);
		b->setAutoRaise(false);
		group_->addButton(b);
		row->addWidget(b);
		connect(b, &QToolButton::clicked, this, [this, scope] { session_->setScope(scope); });
		return b;
	};
	wait_ = button(tr("This wait"), Session::Scope::Wait);
	frame_ = button(tr("This frame"), Session::Scope::Frame);
	sel_ = button(tr("Selected"), Session::Scope::Selection);
	all_ = button(tr("All hitches"), Session::Scope::All);
	row->addStretch(1);
	layout->addLayout(row);

	auto *hint = new QLabel(tr("Width is time. Each row down is one hop of the chain: who the thread above "
				   "was waiting for, and what that thread was doing. Click a box to zoom in."));
	hint->setWordWrap(true);
	hint->setForegroundRole(QPalette::PlaceholderText);
	layout->addWidget(hint);

	crumbs_ = new QLabel;
	crumbs_->setWordWrap(true);
	crumbs_->setTextFormat(Qt::RichText);
	connect(crumbs_, &QLabel::linkActivated, this, [this](const QString &link) {
		session_->setZoom(session_->zoom().mid(0, link.mid(1).toInt()));
	});
	layout->addWidget(crumbs_);

	auto *split = new QSplitter(Qt::Vertical);
	scroll_ = new QScrollArea;
	scroll_->setWidgetResizable(true);
	scroll_->setFrameShape(QFrame::NoFrame);
	scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	view_ = new BlameView(session);
	scroll_->setWidget(view_);
	split->addWidget(scroll_);
	table_ = new QTreeWidget;
	table_->setColumnCount(3);
	table_->setHeaderLabels({ tr("Chain"), tr("ms"), tr("Share") });
	table_->header()->setStretchLastSection(false);
	table_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	table_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	table_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	table_->setUniformRowHeights(true);
	split->addWidget(table_);
	split->setStretchFactor(0, 3);
	split->setStretchFactor(1, 2);
	layout->addWidget(split, 1);

	connect(session, &Session::changed, this, &BlamePanel::refresh);
	refresh();
}

void BlamePanel::refresh()
{
	const Run *r = session_->run();
	const int nSel = int(session_->rangeFrames().size());
	wait_->setVisible(session_->wait() != nullptr);
	sel_->setEnabled(nSel > 0);
	sel_->setText(nSel ? tr("Selected (%1)").arg(nSel) : tr("Selected"));
	all_->setText(r ? tr("All hitches (%1)").arg(r->frames.size()) : tr("All hitches"));
	switch (session_->scope()) {
	case Session::Scope::Wait: wait_->setChecked(true); break;
	case Session::Scope::Frame: frame_->setChecked(true); break;
	case Session::Scope::Selection: sel_->setChecked(true); break;
	case Session::Scope::All: all_->setChecked(true); break;
	}

	const BlameNode *tree = session_->scopeTree();
	QString html;
	if (tree) {
		html = session_->zoom().isEmpty() ? tree->label.toHtmlEscaped()
						  : QStringLiteral("<a href=\"z0\">%1</a>").arg(tree->label.toHtmlEscaped());
		const BlameNode *n = tree;
		const QStringList &z = session_->zoom();
		for (int i = 0; i < z.size(); i++) {
			auto it = n->byKey.find(z[i]);
			if (it == n->byKey.end())
				break;
			n = *it;
			html += QStringLiteral(" › ");
			html += i + 1 < z.size() ? QStringLiteral("<a href=\"z%1\">%2</a>").arg(i + 1).arg(n->label.toHtmlEscaped())
						 : n->label.toHtmlEscaped();
		}
	}
	// the trail only matters once zoomed in; the top box already names the scope
	crumbs_->setText(html);
	crumbs_->setVisible(!session_->zoom().isEmpty());
	// the icicle never needs more room than its rows; the table gets the rest
	scroll_->setMaximumHeight(view_->height() + 2);
	if (session_->treeGeneration() != tableGen_ || session_->zoom() != tableZoom_)
		fillTable();
}

void BlamePanel::fillTable()
{
	table_->clear();
	const BlameNode *tree = session_->scopeTree();
	tableGen_ = session_->treeGeneration();
	tableZoom_ = session_->zoom();
	if (!tree)
		return;
	const BlameNode *top = tree;
	for (const auto &k : tableZoom_) {
		auto it = top->byKey.find(k);
		if (it == top->byKey.end())
			break;
		top = *it;
	}
	const Theme t = Theme::current();
	std::function<void(QTreeWidgetItem *, const BlameNode *, int)> add =
		[&](QTreeWidgetItem *parent, const BlameNode *n, int depth) {
			for (const BlameNode *c : n->sortedKids()) {
				auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(table_);
				item->setText(0, c->label);
				item->setText(1, QString::number(c->ns / 1e6, 'f', 1));
				item->setText(2, QStringLiteral("%1%").arg(tree->ns ? 100.0 * c->ns / tree->ns : 0, 0, 'f', 1));
				item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
				item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
				QPixmap swatch(10, 10);
				swatch.fill(t.fill(c->cat));
				item->setIcon(0, QIcon(swatch));
				add(item, c, depth + 1);
				item->setExpanded(depth < 1);
			}
		};
	add(nullptr, top, 0);
}

}  // namespace ht
