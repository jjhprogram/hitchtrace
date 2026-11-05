// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// The blame tree as an icicle: width is time, each row down is one hop of the
// chain. Hover a box to light its part of the critical path; click to zoom.
#pragma once

#include "ui/session.h"

#include <QWidget>

namespace ht {

class BlameView : public QWidget {
	Q_OBJECT
public:
	explicit BlameView(Session *session, QWidget *parent = nullptr);
	QSize sizeHint() const override { return { 520, height() }; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	struct Box {
		QRectF rect;
		const BlameNode *node;
		QStringList keys;	// from the tree's root
		bool top;
	};
	void relayout();
	const Box *boxAt(QPointF pos) const;

	Session *session_;
	std::vector<Box> boxes_;	// filled while painting
};

}  // namespace ht
