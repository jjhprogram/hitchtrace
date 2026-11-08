// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// The main window: the session strip docked on top, the selected frame in the
// middle (critical path, then thread lanes), the blame tree docked right. Docks
// can be moved, floated and tabbed; the layout is remembered.
#pragma once

#include "ui/session.h"

#include <QMainWindow>

class QComboBox;
class QLabel;
class QPushButton;
class QDockWidget;

namespace ht {

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(Session *session, QWidget *parent = nullptr);
	// Loads each file as a run; reports failures. Returns how many loaded.
	int openFiles(const QStringList &paths);
	// Opens the frame with this id in the current run, if there is one.
	bool showFrameId(std::uint64_t id);
	// Opens the Record dialog, with this process selected if it is listed.
	void recordDialog(int pid = 0);

protected:
	void closeEvent(QCloseEvent *e) override;
	void changeEvent(QEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;

private:
	void refresh();
	void refreshRuns();
	void refreshLegend();
	void openDialog();

	Session *session_;
	QComboBox *runs_;
	QLabel *frameTitle_, *frameOver_, *legend_, *waitNote_, *status_;
	QWidget *waitBar_;
	QAction *prev_, *next_, *follow_;
	QDockWidget *stripDock_, *blameDock_, *stackDock_;
	QByteArray defaultState_;
};

}  // namespace ht
