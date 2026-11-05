// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/ribbon.h"

#include "ui/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

namespace ht {

namespace {
constexpr double Top = 18, BarH = 32;
}

QString chainHtml(const Segment &s)
{
	QString html = QStringLiteral("<b>%1</b> · %2–%3 ms")
			       .arg(fmtMs(s.b - s.a))
			       .arg(s.a / 1e6, 0, 'f', 1)
			       .arg(s.b / 1e6, 0, 'f', 1);
	for (size_t i = 0; i < s.path.size(); i++) {
		const Node &n = s.path[i];
		html += QStringLiteral("<br>%1%2%3")
				.arg(i ? QStringLiteral("→ ") : QString())
				.arg(n.label.toHtmlEscaped())
				.arg(n.via.isEmpty() ? QString() : QStringLiteral(" (on %1)").arg(n.via));
	}
	if (!s.last().ext.isEmpty())
		html += QStringLiteral("<br><i>%1 is another process: hitchtrace has its name and how long "
				       "it held things up, not what it did.</i>")
				.arg(s.last().ext.toHtmlEscaped());
	return html;
}

Ribbon::Ribbon(Session *session, QWidget *parent) : QWidget(parent), session_(session)
{
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(session, &Session::changed, this, [this] {
		if (session_->frame() != for_) {
			for_ = session_->frame();
			segs_ = for_ ? merged(criticalPath(*session_->run(), *for_)) : std::vector<Segment>{};
		}
		update();
	});
}

double Ribbon::x(std::int64_t ns) const
{
	const Frame *f = session_->frame();
	return f ? 1 + double(ns) / f->duration * (width() - 2) : 0;
}

int Ribbon::segmentAt(QPointF pos) const
{
	if (pos.y() < Top || pos.y() > Top + BarH)
		return -1;
	for (int i = 0; i < int(segs_.size()); i++)
		if (pos.x() >= x(segs_[i].a) && pos.x() < x(segs_[i].b))
			return i;
	return -1;
}

void Ribbon::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const Theme t = Theme::current();
	p.fillRect(rect(), t.surface);
	const Run *r = session_->run();
	const Frame *f = session_->frame();
	if (!r || !f)
		return;
	QFont small = font();
	small.setPointSizeF(font().pointSizeF() * 0.85);
	p.setFont(small);
	if (!f->hasTimeline) {
		p.setPen(t.muted);
		p.drawText(rect(), Qt::AlignCenter,
			   QStringLiteral("This record has no timeline (hitchtrace ran with --no-timeline)."));
		return;
	}
	for (const auto &s : segs_) {
		const double x0 = x(s.a), w = std::max(0.5, x(s.b) - x0 - (x(s.b) - x0 > 4 ? 2 : 0));
		const QRectF rr(x0, Top, w, BarH);
		p.setOpacity(session_->lit(s) ? 1.0 : 0.25);
		p.setRenderHint(QPainter::Antialiasing, true);
		QPainterPath path;
		path.addRoundedRect(rr, 2, 2);
		p.fillPath(path, t.fill(s.cat));
		if (!s.last().ext.isEmpty())
			p.fillPath(path, t.hatch(true));
		p.setRenderHint(QPainter::Antialiasing, false);
		if (session_->selectedSegment() == Session::segmentKey(s)) {
			p.setPen(QPen(t.path, 2));
			p.drawRect(rr.adjusted(-1, -1, 1, 1));
		}
		const QString nm = threadName(*r, *f, s.tid);
		const QString label = s.path.size() > 1 || s.cat != Cat::Run ? nm + " · " + s.last().shortLabel
									     : nm + " running";
		const QString shown = elided(small, label, int(w) - 10);
		if (!shown.isEmpty()) {
			p.setPen(t.onFill(s.cat));
			p.drawText(rr.adjusted(5, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, shown);
		}
	}
	p.setOpacity(1);
	// the budget, and a millisecond scale
	if (r->budget < f->duration) {
		p.setPen(QPen(t.ink, 1, Qt::DashLine));
		p.drawLine(QPointF(x(r->budget), Top - 4), QPointF(x(r->budget), Top + BarH + 4));
		p.setPen(t.muted);
		p.drawText(QPointF(x(r->budget) + 3, Top - 6), QStringLiteral("budget"));
	}
	p.setPen(t.muted);
	const std::int64_t step = f->duration > 60'000'000 ? 20'000'000 : f->duration > 30'000'000 ? 10'000'000 : 5'000'000;
	for (std::int64_t v = 0; v <= f->duration; v += step) {
		const QString s = QString::number(v / 1'000'000) + (v ? QString() : QStringLiteral(" ms"));
		p.drawText(QRectF(x(v) - (v ? 30 : 0), Top + BarH + 6, 60, 16),
			   (v ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignTop, s);
	}
}

void Ribbon::mouseMoveEvent(QMouseEvent *e)
{
	const int i = segmentAt(e->position());
	if (i < 0) {
		QToolTip::hideText();
		return;
	}
	const Segment &s = segs_[i];
	QString html = chainHtml(s);
	const Frame *f = session_->frame();
	if (s.tid == f->rootTid && s.path.size() == 1 && s.cat == Cat::Run) {
		const RootKernel k = rootKernel(*session_->run(), *f);
		if (k.kernel)
			html += QStringLiteral("<br><i>Of the presenting thread's running time in this frame, %1 was "
					       "in the kernel. The record has the amount, not when.</i>")
					.arg(fmtMs(k.kernel));
	}
	QToolTip::showText(e->globalPosition().toPoint(), html, this);
}

void Ribbon::mousePressEvent(QMouseEvent *e)
{
	const int i = segmentAt(e->position());
	if (i >= 0)
		session_->selectSegment(Session::segmentKey(segs_[i]));
}

void Ribbon::leaveEvent(QEvent *)
{
	QToolTip::hideText();
}

}  // namespace ht
