// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// The blame dock: what the tree covers (this wait, this frame, the selected
// frames, all hitches), where it is zoomed to, the icicle, and the same tree
// as a table.
#pragma once

#include "ui/session.h"

#include <QWidget>

class QButtonGroup;
class QLabel;
class QScrollArea;
class QToolButton;
class QTreeWidget;

namespace ht {

class BlameView;

class BlamePanel : public QWidget {
	Q_OBJECT
public:
	explicit BlamePanel(Session *session, QWidget *parent = nullptr);

private:
	void refresh();
	void fillTable();

	Session *session_;
	QToolButton *wait_, *frame_, *sel_, *all_;
	QButtonGroup *group_;
	QLabel *crumbs_;
	QTreeWidget *table_;
	QScrollArea *scroll_;
	BlameView *view_;
	std::uint64_t tableGen_ = ~0ull;
	QStringList tableZoom_;
};

}  // namespace ht
