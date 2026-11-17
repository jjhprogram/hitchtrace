// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// One lane per thread of the game across the frame: grey while it ran,
// coloured while it was off the CPU, by what it waited for. The path being
// followed is lit and outlined across lanes; other processes on it get
// hatched placeholder lanes. Click a wait to follow that wait's own chain.
// The lanes keep one order per frame (the presenting thread, the threads on
// the frame's critical path, then the rest by name), so following a wait
// never moves them.
//
// Time can be zoomed: Ctrl+wheel zooms about the pointer, a left-button drag
// or Shift+wheel pans, and a double-click on the time scale shows the whole
// frame again. A new frame always opens whole.
#pragma once

#include "ui/session.h"

#include <QWidget>
#include <optional>

namespace ht {

class Lanes : public QWidget {
	Q_OBJECT
public:
	explicit Lanes(Session *session, QWidget *parent = nullptr);
	QSize sizeHint() const override { return { 700, height() }; }

	// The stretch of the frame on show, in ns from its start.
	std::pair<std::int64_t, std::int64_t> view() const { return { viewA_, viewB_ }; }
	bool zoomed() const;
	// Zoom by @factor (above 1 zooms in), keeping @anchor, a time in the
	// frame, where it is on screen.
	void zoomBy(double factor, std::int64_t anchor);
	void resetZoom();

protected:
	void paintEvent(QPaintEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	void relayout();
	void setView(std::int64_t a, std::int64_t b);
	double x(std::int64_t ns) const;
	std::int64_t timeAt(double px) const;
	double labelWidth() const { return width() < 560 ? 110 : 170; }
	double plotLeft() const { return labelWidth() + 8; }
	double plotRight() const { return width() - 6; }
	const Event *eventAt(QPointF pos) const;

	Session *session_;
	std::vector<std::int32_t> order_;	// lanes for the game's threads
	QStringList ghosts_;			// lanes for other processes on the path
	std::vector<std::pair<QRectF, const Event *>> hits_;	// filled while painting

	const Frame *viewFrame_ = nullptr;	// the frame the view below belongs to
	std::int64_t viewA_ = 0, viewB_ = 0;
	// a left-button press that moves is a pan; one that does not is a click
	std::optional<QPointF> pressPos_;
	std::int64_t pressA_ = 0, pressB_ = 0;
	bool dragging_ = false;
};

}  // namespace ht
