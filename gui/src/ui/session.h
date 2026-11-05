// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// What the user is looking at, shared by every view: the open recordings, the
// frame, a range of frames, a wait being followed, what the blame tree covers,
// and what is highlighted. Views read it and repaint on changed().
#pragma once

#include "analysis/analysis.h"

#include <QObject>
#include <QStringList>
#include <memory>
#include <optional>
#include <vector>

namespace ht {

class Session : public QObject {
	Q_OBJECT
public:
	enum class Scope { Frame, Selection, All, Wait };

	explicit Session(QObject *parent = nullptr);

	void addRun(std::unique_ptr<Run> run);
	const std::vector<std::unique_ptr<Run>> &runs() const { return runs_; }
	int runIndex() const { return run_; }
	const Run *run() const { return run_ >= 0 ? runs_[run_].get() : nullptr; }
	int frameIndex() const { return frame_; }
	const Frame *frame() const;

	bool hasRange() const { return range_.has_value(); }
	std::pair<std::int64_t, std::int64_t> range() const { return range_.value_or(std::pair{ 0, 0 }); }
	std::vector<const Frame *> rangeFrames() const;

	const Event *wait() const { return wait_; }
	Scope scope() const { return scope_; }
	const QStringList &zoom() const { return zoom_; }
	const QStringList &hover() const { return hover_; }
	// The segment picked on the critical path, as segmentKey() of it; empty if none.
	const QString &selectedSegment() const { return segSel_; }
	static QString segmentKey(const Segment &s) { return QStringLiteral("%1:%2").arg(s.tid).arg(s.a); }
	bool follow() const { return follow_; }

	// The path being followed: the frame's critical path, or the chain of the
	// wait being followed; adjacent segments with the same chain joined.
	const std::vector<Segment> &activeSegments();
	// Whether @s lies under the blame node being hovered (or nothing is).
	bool lit(const Segment &s) const;
	// The blame tree for the current scope, and a number that changes whenever
	// that tree is replaced.
	const BlameNode *scopeTree();
	std::uint64_t treeGeneration() const { return gen_; }

public slots:
	void setRun(int index);
	void setFrame(int index);
	void nextFrame();
	void prevFrame();
	void setRange(std::int64_t a, std::int64_t b);
	void clearRange();
	void followWait(const ht::Event *e);	// nullptr: back to the critical path
	void setScope(ht::Session::Scope s);
	void setZoom(const QStringList &keys);
	void setHover(const QStringList &keys);
	void selectSegment(const QString &key);
	void setFollow(bool on);

signals:
	void runsChanged();
	void changed();

private:
	void invalidate(bool tree);

	std::vector<std::unique_ptr<Run>> runs_;
	int run_ = -1;
	int frame_ = -1;
	std::optional<std::pair<std::int64_t, std::int64_t>> range_;
	const Event *wait_ = nullptr;
	Scope scope_ = Scope::Frame;
	QStringList zoom_, hover_;
	QString segSel_;
	bool follow_ = true;

	std::optional<std::vector<Segment>> active_;
	std::unique_ptr<BlameNode> tree_;
	std::uint64_t gen_ = 0;
	std::vector<std::unique_ptr<BlameNode>> allTrees_;	// per run, "all hitches" is slow to build
};

}  // namespace ht
