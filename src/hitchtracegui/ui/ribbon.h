// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// The frame's critical path as one bar the length of the frame: what decided
// when it finished, moment by moment. Hover a part for its chain; click it to
// mark it in the thread lanes.
#pragma once

#include "ui/session.h"

#include <QWidget>

namespace ht {

class Ribbon : public QWidget {
	Q_OBJECT
public:
	explicit Ribbon(Session *session, QWidget *parent = nullptr);
	QSize sizeHint() const override { return { 700, 76 }; }
	QSize minimumSizeHint() const override { return { 300, 76 }; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	double x(std::int64_t ns) const;
	int segmentAt(QPointF pos) const;

	Session *session_;
	std::vector<Segment> segs_;	// the frame's critical path, adjacent parts joined
	const Frame *for_ = nullptr;
};

// The tooltip text for one part of a chain, shared by the views.
QString chainHtml(const Segment &s);

}  // namespace ht
