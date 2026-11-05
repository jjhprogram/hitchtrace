// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// Where the threads on the path were blocked, with their stacks. It follows
// what the rest of the window is focused on: the frame's critical path, the
// part of it picked on the ribbon, the wait being followed, or a box zoomed
// into in the blame tree (over one frame, the selected frames or all of them).
// Each row is a thread and a blocking stack, with the time it cost the path.
#pragma once

#include "ui/session.h"

#include <QWidget>

class QLabel;
class QTreeWidget;

namespace ht {

class StackPanel : public QWidget {
	Q_OBJECT
public:
	explicit StackPanel(Session *session, QWidget *parent = nullptr);

private:
	void refresh();

	Session *session_;
	QLabel *what_, *note_;
	QTreeWidget *tree_;
	QString shownFor_;	// what the tree was last built for
};

}  // namespace ht
