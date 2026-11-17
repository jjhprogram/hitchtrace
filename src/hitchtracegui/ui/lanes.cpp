// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/lanes.h"

#include "ui/ribbon.h"
#include "ui/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

#include <cmath>

namespace ht {

namespace {
constexpr double Top = 22, RowH = 20, Gap = 5, Bottom = 20;
constexpr std::int64_t MinSpan = 20'000;	// the narrowest view: 20 µs
constexpr double DragSlop = 4;			// pixels a press may move and still be a click

// A spacing for the time scale's lines: at most 8 across @span.
std::int64_t tickStep(std::int64_t span)
{
	static const std::int64_t steps[] = {
		1'000, 2'000, 5'000, 10'000, 20'000, 50'000, 100'000, 200'000, 500'000,
		1'000'000, 2'000'000, 5'000'000, 10'000'000, 20'000'000, 50'000'000,
		100'000'000, 200'000'000, 500'000'000, 1'000'000'000,
	};
	for (auto s : steps)
		if (span / s <= 8)
			return s;
	return 2'000'000'000;
}

// A tick's label in ms, with as many decimals as the spacing needs.
QString tickLabel(std::int64_t v, std::int64_t step)
{
	const int decimals = step >= 1'000'000 ? 0 : step >= 100'000 ? 1 : step >= 10'000 ? 2 : 3;
	return QString::number(double(v) / 1e6, 'f', decimals);
}
}

Lanes::Lanes(Session *session, QWidget *parent) : QWidget(parent), session_(session)
{
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(session, &Session::changed, this, [this] {
		relayout();
		update();
	});
	relayout();
}

void Lanes::relayout()
{
	order_.clear();
	ghosts_.clear();
	const Run *r = session_->run();
	const Frame *f = session_->frame();
	if (f != viewFrame_) {
		// a new frame opens whole
		viewFrame_ = f;
		viewA_ = 0;
		viewB_ = f ? f->duration : 0;
	}
	if (r && f) {
		// One order per frame, so that following a wait never moves a lane: the
		// presenting thread, the threads on the frame's critical path in the
		// order it reaches them, then the rest by name.
		order_.push_back(f->rootTid);
		auto add = [&](std::int32_t tid) {
			if (std::find(order_.begin(), order_.end(), tid) == order_.end())
				order_.push_back(tid);
		};
		for (const auto &s : criticalPath(*r, *f))
			add(s.tid);
		std::vector<std::int32_t> rest;
		for (auto tid : frameThreads(*r, *f))
			if (std::find(order_.begin(), order_.end(), tid) == order_.end()
			    && !threadEvents(*r, *f, tid).empty())
				rest.push_back(tid);
		std::sort(rest.begin(), rest.end(), [&](std::int32_t a, std::int32_t b) {
			return threadName(*r, *f, a) < threadName(*r, *f, b);
		});
		order_.insert(order_.end(), rest.begin(), rest.end());
		// A followed wait's chain can pass through a thread that never left the
		// CPU this frame, which has no lane yet: add it at the bottom, below the
		// others, and the other processes on that chain below it.
		for (const auto &s : session_->activeSegments()) {
			add(s.tid);
			if (!s.last().ext.isEmpty() && !ghosts_.contains(s.last().ext))
				ghosts_ << s.last().ext;
		}
	}
	const int rows = int(order_.size()) + int(ghosts_.size());
	setFixedHeight(int(Top + rows * (RowH + Gap) + Bottom));
}

bool Lanes::zoomed() const
{
	const Frame *f = session_->frame();
	return f && (viewA_ > 0 || viewB_ < f->duration);
}

void Lanes::setView(std::int64_t a, std::int64_t b)
{
	const Frame *f = session_->frame();
	if (!f || f->duration <= 0)
		return;
	std::int64_t span = std::clamp<std::int64_t>(b - a, std::min(MinSpan, f->duration), f->duration);
	a = std::clamp<std::int64_t>(a, 0, f->duration - span);
	if (a == viewA_ && a + span == viewB_)
		return;
	viewA_ = a;
	viewB_ = a + span;
	update();
}

void Lanes::zoomBy(double factor, std::int64_t anchor)
{
	if (!session_->frame() || factor <= 0 || viewB_ <= viewA_)
		return;
	const double span = double(viewB_ - viewA_);
	const double frac = std::clamp(double(anchor - viewA_) / span, 0.0, 1.0);
	const double next = span / factor;
	const std::int64_t a = anchor - std::int64_t(std::llround(frac * next));
	setView(a, a + std::int64_t(std::llround(next)));
}

void Lanes::resetZoom()
{
	if (const Frame *f = session_->frame())
		setView(0, f->duration);
}

// Off-screen times map off the plot, a little past its edges: the painter
// clips there, and the coordinates stay small however far in the view is.
double Lanes::x(std::int64_t ns) const
{
	const double x0 = plotLeft(), x1 = plotRight();
	if (viewB_ <= viewA_)
		return x0;
	const double v = x0 + double(ns - viewA_) / double(viewB_ - viewA_) * (x1 - x0);
	return std::clamp(v, x0 - 10, x1 + 10);
}

std::int64_t Lanes::timeAt(double px) const
{
	const double x0 = plotLeft(), x1 = plotRight();
	const double frac = x1 > x0 ? std::clamp((px - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
	return viewA_ + std::int64_t(std::llround(frac * double(viewB_ - viewA_)));
}

void Lanes::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const Theme t = Theme::current();
	p.fillRect(rect(), t.surface);
	hits_.clear();
	const Run *r = session_->run();
	const Frame *f = session_->frame();
	if (!r || !f)
		return;
	QFont small = font();
	small.setPointSizeF(font().pointSizeF() * 0.85);
	p.setFont(small);
	const double lw = labelWidth();
	auto laneY = [&](std::int32_t tid) -> double {
		auto it = std::find(order_.begin(), order_.end(), tid);
		return it == order_.end() ? -1 : Top + (it - order_.begin()) * (RowH + Gap);
	};
	auto ghostY = [&](const QString &c) {
		return Top + (order_.size() + ghosts_.indexOf(c)) * (RowH + Gap);
	};
	const double bottom = height() - Bottom;
	// everything placed in time is drawn inside the plot; names and the scale's text are not
	const QRectF plot(plotLeft() - 1, 0, plotRight() - plotLeft() + 3, height());

	// time scale
	const std::int64_t step = tickStep(viewB_ - viewA_);
	for (std::int64_t v = (viewA_ + step - 1) / step * step; v <= viewB_; v += step) {
		p.setPen(t.grid);
		p.drawLine(QPointF(x(v), Top - 4), QPointF(x(v), bottom));
		p.setPen(t.muted);
		p.drawText(QRectF(x(v) - 30, 2, 60, 16), Qt::AlignHCenter | Qt::AlignVCenter, tickLabel(v, step));
	}
	p.setPen(t.muted);
	p.drawText(QPointF(lw + 8, height() - 5), QStringLiteral("ms into the frame"));
	if (zoomed()) {
		const QString z = QStringLiteral("%1–%2 ms of %3 · double-click the scale for the whole frame")
					  .arg(tickLabel(viewA_, step / 10 ? step / 10 : 1),
					       tickLabel(viewB_, step / 10 ? step / 10 : 1),
					       QString::number(double(f->duration) / 1e6, 'f', 1));
		p.drawText(QRectF(lw + 8, height() - Bottom + 2, plotRight() - lw - 8, Bottom - 4),
			   Qt::AlignRight | Qt::AlignVCenter, z);
	}

	// every lane, dimmed under the path when the path is lit
	const double dim = session_->follow() ? 0.32 : 1.0;
	for (auto tid : order_) {
		const double y0 = laneY(tid);
		p.setOpacity(1);
		p.setPen(t.ink2);
		const QString nm = threadName(*r, *f, tid) + (tid == f->rootTid ? QStringLiteral(" ★") : QString());
		p.drawText(QRectF(0, y0, lw, RowH), Qt::AlignVCenter | Qt::AlignLeft, elided(small, nm, int(lw)));
		p.setClipRect(plot);
		p.setOpacity(dim);
		p.fillRect(QRectF(x(0), y0, x(f->duration) - x(0), RowH), t.fill(Cat::Run));
		for (const Event *e : threadEvents(*r, *f, tid)) {
			if (e->in < viewA_ || e->out > viewB_)
				continue;
			const std::int64_t blockedEnd = e->cause == Cause::Runnable ? e->in : e->wake;
			const double a = x(e->out), b = x(blockedEnd);
			const QRectF rr(a, y0, std::max(1.0, b - a), RowH);
			p.fillRect(rr, t.fill(catOf(e->cause)));
			if (e->cause != Cause::Runnable && e->in > e->wake)
				p.fillRect(QRectF(b, y0 + 4, std::max(0.6, x(e->in) - b), RowH - 8), t.fill(Cat::Sched));
			const QRectF hit = QRectF(a, y0, std::max(3.0, x(e->in) - a), RowH) & plot;
			if (!hit.isEmpty())
				hits_.push_back({ hit, e });
		}
		p.setClipping(false);
	}
	p.setOpacity(1);
	for (const auto &c : ghosts_) {
		const double y0 = ghostY(c);
		QFont it = small;
		it.setItalic(true);
		p.setFont(it);
		p.setPen(t.muted);
		p.drawText(QRectF(0, y0, lw, RowH), Qt::AlignVCenter | Qt::AlignLeft,
			   elided(it, c + QStringLiteral(" (other process)"), int(lw)));
		p.setFont(small);
		p.setPen(QPen(t.grid, 1, Qt::DashLine));
		const double ga = std::max(x(0), plotLeft()), gb = std::min(x(f->duration), plotRight());
		p.drawRect(QRectF(ga, y0, gb - ga, RowH));
	}

	// the path, the wait being followed and the budget line are all placed in time
	p.setClipRect(plot);

	// the path: lit copies of its segments, outlined, joined where it changes thread
	const Segment *prev = nullptr;
	for (const auto &s : session_->activeSegments()) {
		const double ty = laneY(s.tid);
		if (ty < 0)
			continue;
		const double a = x(s.a), w = std::max(1.0, x(s.b) - a);
		p.setOpacity(session_->lit(s) ? 1.0 : 0.35);
		if (session_->follow())
			p.fillRect(QRectF(a, ty, w, RowH), t.fill(s.cat));
		p.setPen(QPen(t.path, 1.6));
		p.setBrush(Qt::NoBrush);
		p.drawRect(QRectF(a, ty, w, RowH));
		if (!s.last().ext.isEmpty()) {
			const double gy = ghostY(s.last().ext);
			p.fillRect(QRectF(a, gy, w, RowH), t.hatch(false));
			p.setPen(QPen(t.path, 1.2, Qt::DashLine));
			p.drawRect(QRectF(a, gy, w, RowH));
			if (w > 70) {
				p.setPen(t.ink2);
				p.drawText(QRectF(a + 4, gy, w - 6, RowH), Qt::AlignVCenter | Qt::AlignLeft,
					   elided(small, s.cat == Cat::Sched ? QStringLiteral("had the CPU")
									    : QStringLiteral("ended the wait"), int(w) - 8));
			}
			p.setPen(QPen(t.path, 1, Qt::DotLine));
			p.drawLine(QPointF(a + w / 2, ty + RowH), QPointF(a + w / 2, gy));
		}
		if (prev && prev->tid != s.tid) {
			const double py = laneY(prev->tid);
			p.setPen(QPen(t.path, 1.6));
			p.drawLine(QPointF(a, std::min(py, ty) + RowH / 2), QPointF(a, std::max(py, ty) + RowH / 2));
			p.setRenderHint(QPainter::Antialiasing, true);
			p.setBrush(t.path);
			p.drawEllipse(QPointF(a, ty + RowH / 2), 2.6, 2.6);
			p.setBrush(Qt::NoBrush);
			p.setRenderHint(QPainter::Antialiasing, false);
		}
		if (session_->selectedSegment() == Session::segmentKey(s)) {
			p.setPen(QPen(t.focus, 2.5));
			p.drawRect(QRectF(a - 2, ty - 2, w + 4, RowH + 4));
		}
		prev = &s;
	}
	p.setOpacity(1);

	// the wait being followed, on its own thread
	if (const Event *e = session_->wait()) {
		const double y0 = laneY(e->tid);
		p.setPen(QPen(t.focus, 2.5, Qt::DashLine));
		p.drawRect(QRectF(x(e->out) - 1, y0 - 2, std::max(2.0, x(e->wake) - x(e->out)) + 2, RowH + 4));
	}
	if (r->budget < f->duration) {
		p.setPen(QPen(t.ink, 1, Qt::DashLine));
		p.drawLine(QPointF(x(r->budget), Top - 2), QPointF(x(r->budget), bottom));
	}
}

const Event *Lanes::eventAt(QPointF pos) const
{
	// the last drawn wins: later intervals sit on top
	for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
		if (it->first.contains(pos))
			return it->second;
	return nullptr;
}

static QString waitHtml(const Run &run, const Frame &f, const Event &e)
{
	const QString nm = threadName(run, f, e.tid).toHtmlEscaped();
	const QString other = e.byTgid && e.byTgid != run.pid ? QStringLiteral(" (another process)") : QString();
	if (e.cause == Cause::Runnable)
		return QStringLiteral("<b>%1: ready, but off the CPU</b><br>%2<br>%3")
			.arg(nm, fmtMs(e.in - e.out),
			     e.byComm.isEmpty() ? QStringLiteral("who had the CPU is not recorded")
						: e.byComm.toHtmlEscaped() + " had the CPU" + other);
	if (e.cause == Cause::Open)
		return QStringLiteral("<b>%1: still off the CPU when the frame ended</b><br>%2")
			.arg(nm, fmtMs(e.wake - e.out));
	const QString ended = e.flags & FlagsInterrupt ? QStringLiteral("ended by an interrupt")
			      : e.byComm.isEmpty()    ? QStringLiteral("nobody on record ended it")
						      : "ended by " + e.byComm.toHtmlEscaped() + other;
	QString html = QStringLiteral("<b>%1: waiting on %2</b><br>%3, %4")
			       .arg(nm, causeWord(e.cause), fmtMs(e.wake - e.out), ended);
	if (e.byStall && !e.byComm.isEmpty())
		html += QStringLiteral("<br>%1 was itself held up %2 of that").arg(e.byComm.toHtmlEscaped(), fmtMs(e.byStall));
	if (e.in > e.wake)
		html += QStringLiteral("<br>then %1 waiting for a CPU").arg(fmtMs(e.in - e.wake));
	if (!e.blockedIn.isEmpty())
		html += QStringLiteral("<br><i>blocked in %1</i>").arg(e.blockedIn.toHtmlEscaped());
	html += QStringLiteral("<br><i>Click to follow this wait's chain</i>");
	return html;
}

void Lanes::mouseMoveEvent(QMouseEvent *e)
{
	if (pressPos_ && (e->buttons() & Qt::LeftButton)) {
		const double dx = e->position().x() - pressPos_->x();
		if (!dragging_ && std::abs(dx) > DragSlop && zoomed()) {
			dragging_ = true;
			setCursor(Qt::ClosedHandCursor);
			QToolTip::hideText();
		}
		if (dragging_) {
			const double w = plotRight() - plotLeft();
			const auto shift = std::int64_t(std::llround(-dx / (w > 0 ? w : 1) * double(pressB_ - pressA_)));
			setView(pressA_ + shift, pressB_ + shift);
			return;
		}
	}
	const Event *ev = eventAt(e->position());
	if (!ev) {
		QToolTip::hideText();
		return;
	}
	QToolTip::showText(e->globalPosition().toPoint(), waitHtml(*session_->run(), *session_->frame(), *ev), this);
}

void Lanes::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	pressPos_ = e->position();
	pressA_ = viewA_;
	pressB_ = viewB_;
	dragging_ = false;
}

void Lanes::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !pressPos_)
		return;
	const bool click = !dragging_;
	const QPointF at = *pressPos_;
	pressPos_.reset();
	if (dragging_) {
		dragging_ = false;
		unsetCursor();
	}
	if (click)
		if (const Event *ev = eventAt(at))
			session_->followWait(ev);
}

void Lanes::mouseDoubleClickEvent(QMouseEvent *e)
{
	// on the time scale: the whole frame again
	if (e->button() == Qt::LeftButton && e->position().y() < Top) {
		resetZoom();
		return;
	}
	// elsewhere a double-click is two clicks, as before
	mousePressEvent(e);
}

void Lanes::wheelEvent(QWheelEvent *e)
{
	if (!session_->frame()) {
		e->ignore();
		return;
	}
	const QPoint d = e->angleDelta();
	if (e->modifiers() & Qt::ControlModifier) {
		// zoom about the time under the pointer; one notch is 25%
		const int notches = d.y() ? d.y() : d.x();
		if (notches)
			zoomBy(std::pow(1.25, notches / 120.0), timeAt(e->position().x()));
		e->accept();
		return;
	}
	// a sideways wheel, or Shift with the wheel, pans; one notch is an eighth of the view
	int side = d.x();
	if (!side && (e->modifiers() & Qt::ShiftModifier))
		side = d.y();
	if (side && zoomed()) {
		const auto shift = std::int64_t(std::llround(-side / 120.0 * double(viewB_ - viewA_) / 8));
		setView(viewA_ + shift, viewB_ + shift);
		e->accept();
		return;
	}
	e->ignore();	// the plain wheel scrolls the lanes up and down
}

void Lanes::leaveEvent(QEvent *)
{
	QToolTip::hideText();
}

}  // namespace ht
