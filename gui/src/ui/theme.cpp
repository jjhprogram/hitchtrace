// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/theme.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPalette>

namespace ht {

static QColor mix(const QColor &a, const QColor &b, double t)
{
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
				a.blueF() + (b.blueF() - a.blueF()) * t);
}

Theme Theme::current()
{
	const QPalette p = QApplication::palette();
	Theme t;
	t.surface = p.color(QPalette::Base);
	t.ink = p.color(QPalette::Text);
	t.dark = t.surface.lightness() < 128;
	t.ink2 = mix(t.ink, t.surface, 0.28);
	t.muted = mix(t.ink, t.surface, 0.48);
	t.grid = mix(t.ink, t.surface, 0.88);
	t.axis = mix(t.ink, t.surface, 0.72);
	t.wash = mix(t.ink, t.surface, 0.95);
	t.path = t.ink;
	t.focus = p.color(QPalette::Highlight);
	t.selFill = t.focus;
	t.selFill.setAlphaF(t.dark ? 0.20f : 0.13f);

	struct Pair { const char *light, *dark; };
	// run, kern, sched, lock, io, sleep, disp, other, open
	static const Pair fills[CatCount] = {
		{ "#c3c2b7", "#5a5954" }, { "#2a78d6", "#3987e5" }, { "#eb6834", "#d95926" },
		{ "#1baf7a", "#199e70" }, { "#eda100", "#c98500" }, { "#e87ba4", "#d55181" },
		{ "#008300", "#008300" }, { "#4a3aa7", "#9085e9" }, { "#e9e8e2", "#2c2c2a" },
	};
	static const Pair texts[CatCount] = {
		{ "#121211", "#f4f4f1" }, { "#ffffff", "#0f0f0e" }, { "#121211", "#0f0f0e" },
		{ "#121211", "#0f0f0e" }, { "#121211", "#0f0f0e" }, { "#121211", "#0f0f0e" },
		{ "#ffffff", "#ffffff" }, { "#ffffff", "#0f0f0e" }, { "#121211", "#f4f4f1" },
	};
	for (int i = 0; i < CatCount; i++) {
		t.cat[i] = QColor(QLatin1String(t.dark ? fills[i].dark : fills[i].light));
		t.catText[i] = QColor(QLatin1String(t.dark ? texts[i].dark : texts[i].light));
	}
	return t;
}

QBrush Theme::hatch(bool overFill) const
{
	QColor c = overFill ? surface : ink;
	c.setAlphaF(overFill ? 0.55f : 0.40f);
	return QBrush(c, Qt::BDiagPattern);
}

QString fmtMs(std::int64_t ns)
{
	const double ms = ns / 1e6;
	return QString::number(ms, 'f', ms >= 100 ? 0 : 1) + QStringLiteral(" ms");
}

QString elided(const QFont &font, const QString &s, int width)
{
	const QFontMetrics fm(font);
	if (width < fm.horizontalAdvance(QStringLiteral("abc…")))
		return {};
	return fm.elidedText(s, Qt::ElideRight, width);
}

}  // namespace ht
