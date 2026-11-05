// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// Every hitch of the run on one time axis, each bar stacked by what its
// critical path was doing. Click a bar to open the frame; drag to select a
// range of frames; Escape or right-click clears the range.
#pragma once

#include "ui/session.h"

#include <QWidget>

namespace ht {

class SessionStrip : public QWidget {
	Q_OBJECT
public:
	explicit SessionStrip(Session *session, QWidget *parent = nullptr);
	QSize sizeHint() const override { return { 900, 150 }; }
	QSize minimumSizeHint() const override { return { 300, 120 }; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void keyPressEvent(QKeyEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	struct Geo {
		double left, right, top, bottom;	// the plot's edges
		std::int64_t ymax, ystep;
		double x(std::int64_t t, std::int64_t span) const { return left + double(t) / span * (right - left); }
		double y(std::int64_t ns) const { return bottom - double(std::min(ns, ymax)) / ymax * (bottom - top); }
	};
	Geo geometry() const;
	std::int64_t timeAt(double x) const;
	int frameAt(double x) const;

	Session *session_;
	bool dragging_ = false;
	double dragX0_ = 0;
	std::int64_t dragT0_ = 0, dragT1_ = 0;
};

}  // namespace ht
