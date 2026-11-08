// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// Colours for the views: neutrals from the application palette, so the app
// follows the desktop's light or dark theme, and one fixed colour per category
// (the palette the HTML views use, validated for colour-blind separation).
#pragma once

#include "analysis/analysis.h"

#include <QBrush>
#include <QColor>
#include <QFont>

namespace ht {

struct Theme {
	bool dark = false;
	QColor surface, ink, ink2, muted, grid, axis, wash, path, focus, selFill;
	QColor cat[CatCount];
	QColor catText[CatCount];	// text drawn on a category's fill

	static Theme current();
	QBrush hatch(bool overFill) const;	// marks another process's time
	QColor fill(Cat c) const { return cat[int(c)]; }
	QColor onFill(Cat c) const { return catText[int(c)]; }
};

QString fmtMs(std::int64_t ns);			// "12.3 ms", "123 ms"
QString elided(const QFont &font, const QString &s, int width);	// "" if nothing fits

}  // namespace ht
