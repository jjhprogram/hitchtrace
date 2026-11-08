// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/lanes.h"

#include "ui/ribbon.h"
#include "ui/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace ht {

namespace {
constexpr double Top = 22, RowH = 20, Gap = 5, Bottom = 20;
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
	if (r && f) {
		// the presenting thread, the wait being followed, the path's threads, then the rest
		order_.push_back(f->rootTid);
		auto add = [&](std::int32_t tid) {
			if (std::find(order_.begin(), order_.end(), tid) == order_.end())
				order_.push_back(tid);
		};
		if (session_->wait())
			add(session_->wait()->tid);
		for (const auto &s : session_->activeSegments()) {
			add(s.tid);
			if (!s.last().ext.isEmpty() && !ghosts_.contains(s.last().ext))
				ghosts_ << s.last().ext;
		}
		std::vector<std::int32_t> rest;
		for (auto tid : frameThreads(*r, *f))
			if (std::find(order_.begin(), order_.end(), tid) == order_.end()
			    && !threadEvents(*r, *f, tid).empty())
				rest.push_back(tid);
		std::sort(rest.begin(), rest.end(), [&](std::int32_t a, std::int32_t b) {
			return threadName(*r, *f, a) < threadName(*r, *f, b);
		});
		order_.insert(order_.end(), rest.begin(), rest.end());
	}
	const int rows = int(order_.size()) + int(ghosts_.size());
	setFixedHeight(int(Top + rows * (RowH + Gap) + Bottom));
}

double Lanes::x(std::int64_t ns) const
{
	const Frame *f = session_->frame();
	if (!f)
		return 0;
	const double x0 = labelWidth() + 8, x1 = width() - 6;
	return x0 + double(std::clamp<std::int64_t>(ns, 0, f->duration)) / f->duration * (x1 - x0);
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

	// time scale
	const std::int64_t step = f->duration > 60'000'000 ? 20'000'000 : f->duration > 30'000'000 ? 10'000'000 : 5'000'000;
	for (std::int64_t v = 0; v <= f->duration; v += step) {
		p.setPen(t.grid);
		p.drawLine(QPointF(x(v), Top - 4), QPointF(x(v), bottom));
		p.setPen(t.muted);
		p.drawText(QRectF(x(v) - 30, 2, 60, 16), Qt::AlignHCenter | Qt::AlignVCenter,
			   QString::number(v / 1'000'000));
	}
	p.drawText(QPointF(lw + 8, height() - 5), QStringLiteral("ms into the frame"));

	// every lane, dimmed under the path when the path is lit
	const double dim = session_->follow() ? 0.32 : 1.0;
	for (auto tid : order_) {
		const double y0 = laneY(tid);
		p.setOpacity(1);
		p.setPen(t.ink2);
		const QString nm = threadName(*r, *f, tid) + (tid == f->rootTid ? QStringLiteral(" ★") : QString());
		p.drawText(QRectF(0, y0, lw, RowH), Qt::AlignVCenter | Qt::AlignLeft, elided(small, nm, int(lw)));
		p.setOpacity(dim);
		p.fillRect(QRectF(x(0), y0, x(f->duration) - x(0), RowH), t.fill(Cat::Run));
		for (const Event *e : threadEvents(*r, *f, tid)) {
			const std::int64_t blockedEnd = e->cause == Cause::Runnable ? e->in : e->wake;
			const double a = x(e->out), b = x(blockedEnd);
			const QRectF rr(a, y0, std::max(1.0, b - a), RowH);
			p.fillRect(rr, t.fill(catOf(e->cause)));
			if (e->cause != Cause::Runnable && e->in > e->wake)
				p.fillRect(QRectF(b, y0 + 4, std::max(0.6, x(e->in) - b), RowH - 8), t.fill(Cat::Sched));
			hits_.push_back({ QRectF(a, y0, std::max(3.0, x(e->in) - a), RowH), e });
		}
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
		p.drawRect(QRectF(x(0), y0, x(f->duration) - x(0), RowH));
	}

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
	if (const Event *ev = eventAt(e->position()))
		session_->followWait(ev);
}

void Lanes::leaveEvent(QEvent *)
{
	QToolTip::hideText();
}

}  // namespace ht
