// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/blame_view.h"

#include "ui/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QToolTip>
#include <functional>

namespace ht {

namespace {
constexpr double RowH = 26, Gap = 2;

const BlameNode *zoomed(const BlameNode *tree, const QStringList &zoom, QStringList *keys)
{
	const BlameNode *n = tree;
	for (const auto &k : zoom) {
		auto it = n->byKey.find(k);
		if (it == n->byKey.end())
			break;
		n = *it;
		*keys << k;
	}
	return n;
}

int depthOf(const BlameNode *n)
{
	int d = 0;
	for (const auto &k : n->kids)
		d = std::max(d, 1 + depthOf(k.get()));
	return d;
}
}  // namespace

BlameView::BlameView(Session *session, QWidget *parent) : QWidget(parent), session_(session)
{
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(session, &Session::changed, this, [this] {
		relayout();
		update();
	});
	relayout();
}

void BlameView::relayout()
{
	const BlameNode *tree = session_->scopeTree();
	int depth = 0;
	if (tree) {
		QStringList keys;
		depth = depthOf(zoomed(tree, session_->zoom(), &keys));
	}
	setFixedHeight(int((depth + 1) * (RowH + Gap) + 4));
}

void BlameView::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const Theme t = Theme::current();
	p.fillRect(rect(), t.surface);
	boxes_.clear();
	const BlameNode *tree = session_->scopeTree();
	if (!tree)
		return;
	QFont small = font();
	small.setPointSizeF(font().pointSizeF() * 0.85);
	p.setFont(small);
	QStringList zkeys;
	const BlameNode *top = zoomed(tree, session_->zoom(), &zkeys);
	const bool waitScope = session_->scope() == Session::Scope::Wait;

	std::function<void(const BlameNode *, double, double, int, const QStringList &)> place =
		[&](const BlameNode *n, double x0, double w, int d, const QStringList &keys) {
			const double y = d * (RowH + Gap);
			const bool isTop = d == 0;
			const QRectF rr(x0, y, std::max(0.5, w - (w > 4 ? Gap : 0)), RowH);
			p.setRenderHint(QPainter::Antialiasing, true);
			QPainterPath path;
			path.addRoundedRect(rr, 2, 2);
			if (isTop) {
				p.fillPath(path, t.wash);
				p.setPen(QPen(t.grid, 1));
				p.drawPath(path);
			} else {
				p.fillPath(path, t.fill(n->cat));
				if (!n->ext.isEmpty())
					p.fillPath(path, t.hatch(true));
			}
			p.setRenderHint(QPainter::Antialiasing, false);
			const QString label = isTop ? (n == tree ? tree->label : n->label) + " · " + fmtMs(n->ns)
							      + (waitScope ? QString() : QStringLiteral(" on the critical path"))
						    : n->label;
			const QString shown = elided(small, label, int(rr.width()) - 10);
			if (!shown.isEmpty()) {
				p.setPen(isTop ? t.ink : t.onFill(n->cat));
				p.drawText(rr.adjusted(5, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, shown);
			}
			boxes_.push_back({ rr, n, keys, isTop });
			double cx = x0;
			for (const BlameNode *c : n->sortedKids()) {
				const double cw = n->ns ? double(c->ns) / n->ns * w : 0;
				if (cw >= 0.5)
					place(c, cx, cw, d + 1, keys + QStringList{ c->key });
				cx += cw;
			}
		};
	place(top, 0, width(), 0, zkeys);
}

const BlameView::Box *BlameView::boxAt(QPointF pos) const
{
	for (auto it = boxes_.rbegin(); it != boxes_.rend(); ++it)
		if (it->rect.contains(pos))
			return &*it;
	return nullptr;
}

void BlameView::mouseMoveEvent(QMouseEvent *e)
{
	const Box *b = boxAt(e->position());
	const BlameNode *tree = session_->scopeTree();
	if (!b || !tree) {
		QToolTip::hideText();
		session_->setHover({});
		return;
	}
	const BlameNode *n = b->node;
	const bool multi = tree->frameCount > 1;
	QString html = QStringLiteral("<b>%1</b><br>%2 · %3% of %4")
			       .arg((n == tree ? tree->label : n->label).toHtmlEscaped(), fmtMs(n->ns))
			       .arg(tree->ns ? 100.0 * n->ns / tree->ns : 0, 0, 'f', 0)
			       .arg(multi ? QStringLiteral("these hitches' time") : QStringLiteral("the frame"));
	if (multi && n != tree)
		html += QStringLiteral("<br>in %1 of %2 frames").arg(n->frames.size()).arg(tree->frameCount);
	if (!n->via.isEmpty())
		html += QStringLiteral("<br><i>the wait was on %1</i>").arg(n->via);
	if (!n->ext.isEmpty())
		html += QStringLiteral("<br><i>%1 is another process: its own activity is not recorded</i>")
				.arg(n->ext.toHtmlEscaped());
	if (!n->kids.empty() && !b->top)
		html += QStringLiteral("<br><i>Click to zoom in</i>");
	QToolTip::showText(e->globalPosition().toPoint(), html, this);

	// light the matching part of the path (one frame or one wait only)
	const auto scope = session_->scope();
	if (scope == Session::Scope::Frame || scope == Session::Scope::Wait) {
		static const QRegularExpression kernelSplit(QStringLiteral("^(kern\\d|code)$"));
		QStringList keys = b->keys;
		keys.removeIf([](const QString &k) { return kernelSplit.match(k).hasMatch(); });
		session_->setHover(n == tree ? QStringList() : keys);
	}
}

void BlameView::mousePressEvent(QMouseEvent *e)
{
	const Box *b = boxAt(e->position());
	if (!b || b->top || b->node->kids.empty())
		return;
	QToolTip::hideText();
	session_->setZoom(b->keys);
}

void BlameView::leaveEvent(QEvent *)
{
	QToolTip::hideText();
	session_->setHover({});
}

}  // namespace ht
