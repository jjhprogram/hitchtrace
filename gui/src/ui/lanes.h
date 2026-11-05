// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// One lane per thread of the game across the frame: grey while it ran,
// coloured while it was off the CPU, by what it waited for. The path being
// followed is lit and outlined across lanes; other processes on it get
// hatched placeholder lanes. Click a wait to follow that wait's own chain.
#pragma once

#include "ui/session.h"

#include <QWidget>

namespace ht {

class Lanes : public QWidget {
	Q_OBJECT
public:
	explicit Lanes(Session *session, QWidget *parent = nullptr);
	QSize sizeHint() const override { return { 700, height() }; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	void relayout();
	double x(std::int64_t ns) const;
	double labelWidth() const { return width() < 560 ? 110 : 170; }
	const Event *eventAt(QPointF pos) const;

	Session *session_;
	std::vector<std::int32_t> order_;	// lanes for the game's threads
	QStringList ghosts_;			// lanes for other processes on the path
	std::vector<std::pair<QRectF, const Event *>> hits_;	// filled while painting
};

}  // namespace ht
