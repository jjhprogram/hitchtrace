// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/session_strip.h"

#include "ui/theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <cmath>

namespace ht {

SessionStrip::SessionStrip(Session *session, QWidget *parent) : QWidget(parent), session_(session)
{
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	connect(session, &Session::changed, this, qOverload<>(&QWidget::update));
}

SessionStrip::Geo SessionStrip::geometry() const
{
	Geo g;
	g.left = 44;
	g.right = width() - (width() < 560 ? 10 : 64);
	g.top = 16;
	g.bottom = height() - 22;
	const Run *r = session_->run();
	std::int64_t maxD = 1;
	if (r)
		for (const auto &f : r->frames)
			maxD = std::max(maxD, f.duration);
	const std::int64_t cap = r ? std::min(maxD, r->budget * 5) : maxD;
	g.ystep = cap > 120'000'000 ? 50'000'000 : cap > 60'000'000 ? 20'000'000 : 10'000'000;
	g.ymax = std::max<std::int64_t>(g.ystep, (cap + g.ystep - 1) / g.ystep * g.ystep);
	return g;
}

std::int64_t SessionStrip::timeAt(double x) const
{
	const Geo g = geometry();
	const Run *r = session_->run();
	if (!r)
		return 0;
	return std::int64_t((x - g.left) / (g.right - g.left) * r->span);
}

int SessionStrip::frameAt(double x) const
{
	const Run *r = session_->run();
	if (!r)
		return -1;
	const Geo g = geometry();
	int best = -1;
	double bd = 1e18;
	for (int i = 0; i < int(r->frames.size()); i++) {
		const auto &f = r->frames[i];
		const double d = std::abs(g.x(f.start + f.duration / 2, r->span) - x);
		if (d < bd) {
			bd = d;
			best = i;
		}
	}
	return bd <= 8 ? best : -1;
}

void SessionStrip::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);
	const Theme t = Theme::current();
	p.fillRect(rect(), t.surface);
	const Run *r = session_->run();
	if (!r)
		return;
	const Geo g = geometry();
	QFont small = font();
	small.setPointSizeF(font().pointSizeF() * 0.85);
	p.setFont(small);

	// grid and axes
	for (std::int64_t v = 0; v <= g.ymax; v += g.ystep) {
		p.setPen(t.grid);
		p.drawLine(QPointF(g.left, g.y(v)), QPointF(g.right, g.y(v)));
		p.setPen(t.muted);
		p.drawText(QRectF(0, g.y(v) - 8, g.left - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
			   QString::number(v / 1'000'000) + (v == g.ymax ? QStringLiteral(" ms") : QString()));
	}
	const double secs = r->span / 1e9;
	const int tstep = secs > 120 ? 30 : secs > 40 ? 10 : 5;
	for (int s = 0; s <= secs; s += tstep) {
		const double x = g.x(std::int64_t(s) * 1'000'000'000, r->span);
		p.drawText(QRectF(x - 30, g.bottom + 4, 60, 16), Qt::AlignHCenter | Qt::AlignTop,
			   QStringLiteral("%1 s").arg(s));
	}

	// the selected range, and one being dragged
	auto band = [&](std::int64_t a, std::int64_t b) {
		QRectF rr(QPointF(g.x(std::max<std::int64_t>(0, a), r->span), g.top - 6),
			  QPointF(g.x(std::min(r->span, b), r->span), g.bottom));
		p.fillRect(rr, t.selFill);
		p.setPen(QPen(t.focus, 1));
		p.drawRect(rr);
	};
	if (dragging_)
		band(std::min(dragT0_, dragT1_), std::max(dragT0_, dragT1_));
	else if (session_->hasRange())
		band(session_->range().first, session_->range().second);

	// the bars, stacked in legend order
	const double bw = std::clamp((g.right - g.left) / r->frames.size() * 0.7, 2.0, 6.0);
	for (int i = 0; i < int(r->frames.size()); i++) {
		const Frame &f = r->frames[i];
		const auto &tot = frameTotals(*r, f);
		const double cx = g.x(f.start + f.duration / 2, r->span);
		std::int64_t acc = 0;
		for (int c = 0; c < CatCount; c++) {
			if (!tot[c])
				continue;
			const double y0 = g.y(acc), y1 = g.y(acc + tot[c]);
			acc += tot[c];
			if (y0 - y1 < 0.3)
				continue;
			p.fillRect(QRectF(cx - bw / 2, y1, bw, y0 - y1), t.fill(Cat(c)));
		}
		if (f.duration > g.ymax)	// clipped: a cap on top
			p.fillRect(QRectF(cx - bw / 2 - 1, g.top - 4, bw + 2, 2), t.ink);
		if (i == session_->frameIndex()) {
			QPainterPath tri;
			tri.moveTo(cx - 5, g.top - 14);
			tri.lineTo(cx + 5, g.top - 14);
			tri.lineTo(cx, g.top - 7);
			tri.closeSubpath();
			p.setRenderHint(QPainter::Antialiasing, true);
			p.fillPath(tri, t.ink);
			p.setRenderHint(QPainter::Antialiasing, false);
		}
	}

	// the budget
	p.setPen(QPen(t.ink2, 1, Qt::DashLine));
	p.drawLine(QPointF(g.left, g.y(r->budget)), QPointF(g.right, g.y(r->budget)));
	if (width() >= 560) {
		p.setPen(t.muted);
		p.drawText(QPointF(g.right + 6, g.y(r->budget) + 4), QStringLiteral("budget"));
	}
}

void SessionStrip::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::RightButton) {
		session_->clearRange();
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;
	dragging_ = false;
	dragX0_ = e->position().x();
	dragT0_ = dragT1_ = timeAt(dragX0_);
}

void SessionStrip::mouseMoveEvent(QMouseEvent *e)
{
	const double x = e->position().x();
	if (e->buttons() & Qt::LeftButton) {
		if (std::abs(x - dragX0_) > 4) {
			dragging_ = true;
			dragT1_ = timeAt(x);
			QToolTip::hideText();
			update();
		}
		return;
	}
	const Run *r = session_->run();
	const int i = frameAt(x);
	if (!r || i < 0) {
		QToolTip::hideText();
		return;
	}
	const Frame &f = r->frames[i];
	const auto &tot = frameTotals(*r, f);
	std::vector<int> cats;
	for (int c = 0; c < CatCount; c++)
		if (tot[c] >= 500'000)
			cats.push_back(c);
	std::sort(cats.begin(), cats.end(), [&](int a, int b) { return tot[a] > tot[b]; });
	QString html = QStringLiteral("<b>Frame %1: %2</b><br>%3 s into the run")
			       .arg(f.id).arg(fmtMs(f.duration)).arg(f.start / 1e9, 0, 'f', 2);
	for (int c : cats)
		html += QStringLiteral("<br>%1 %2").arg(fmtMs(tot[c]), catLabel(Cat(c)));
	QToolTip::showText(e->globalPosition().toPoint(), html, this);
}

void SessionStrip::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	if (dragging_) {
		dragging_ = false;
		session_->setRange(dragT0_, dragT1_);
		return;
	}
	const int i = frameAt(e->position().x());
	if (i >= 0)
		session_->setFrame(i);
}

void SessionStrip::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape)
		session_->clearRange();
	else
		QWidget::keyPressEvent(e);
}

void SessionStrip::leaveEvent(QEvent *)
{
	QToolTip::hideText();
}

}  // namespace ht
