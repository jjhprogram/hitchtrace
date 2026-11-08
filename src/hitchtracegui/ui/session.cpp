// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "ui/session.h"

namespace ht {

Session::Session(QObject *parent) : QObject(parent) {}

void Session::addRun(std::unique_ptr<Run> run)
{
	runs_.push_back(std::move(run));
	allTrees_.emplace_back();
	emit runsChanged();
	setRun(int(runs_.size()) - 1);
}

const Frame *Session::frame() const
{
	const Run *r = run();
	return r && frame_ >= 0 && frame_ < int(r->frames.size()) ? &r->frames[frame_] : nullptr;
}

std::vector<const Frame *> Session::rangeFrames() const
{
	std::vector<const Frame *> v;
	if (!range_ || !run())
		return v;
	for (const auto &f : run()->frames)
		if (f.start + f.duration >= range_->first && f.start <= range_->second)
			v.push_back(&f);
	return v;
}

void Session::invalidate(bool tree)
{
	active_.reset();
	if (tree) {
		tree_.reset();
		gen_++;
	}
}

const std::vector<Segment> &Session::activeSegments()
{
	if (!active_) {
		const Frame *f = frame();
		if (!f)
			active_.emplace();
		else if (wait_)
			active_ = merged(explainWait(*run(), *f, *wait_));
		else
			active_ = merged(criticalPath(*run(), *f));
	}
	return *active_;
}

bool Session::lit(const Segment &s) const
{
	if (hover_.isEmpty())
		return true;
	if (hover_.size() > int(s.path.size()))
		return false;
	for (int i = 0; i < hover_.size(); i++)
		if (s.path[i].key != hover_[i])
			return false;
	return true;
}

const BlameNode *Session::scopeTree()
{
	const Run *r = run();
	const Frame *f = frame();
	if (!r || !f)
		return nullptr;
	if (scope_ == Scope::All) {
		auto &t = allTrees_[run_];
		if (!t)
			t = blameFrames(*r, [&] {
				std::vector<const Frame *> v;
				for (const auto &fr : r->frames)
					v.push_back(&fr);
				return v;
			}(), QStringLiteral("All %1 hitches").arg(r->frames.size()));
		return t.get();
	}
	if (!tree_) {
		if (scope_ == Scope::Wait && wait_) {
			tree_ = blameWait(*r, *f, *wait_, threadName(*r, *f, wait_->tid) + QStringLiteral("'s wait"));
		} else if (scope_ == Scope::Selection && range_) {
			const auto fr = rangeFrames();
			tree_ = blameFrames(*r, fr, QStringLiteral("%1 selected hitches").arg(fr.size()));
		} else {
			tree_ = blameFrames(*r, { f }, QStringLiteral("Frame %1").arg(f->id));
		}
	}
	return tree_.get();
}

void Session::setRun(int index)
{
	if (index < 0 || index >= int(runs_.size()))
		return;
	run_ = index;
	frame_ = runs_[index]->frames.empty() ? -1 : 0;
	range_.reset();
	wait_ = nullptr;
	scope_ = Scope::Frame;
	zoom_.clear();
	hover_.clear();
	segSel_.clear();
	invalidate(true);
	emit changed();
}

void Session::setFrame(int index)
{
	const Run *r = run();
	if (!r || index < 0 || index >= int(r->frames.size()))
		return;
	frame_ = index;
	wait_ = nullptr;
	if (scope_ == Scope::Wait)
		scope_ = Scope::Frame;
	if (scope_ == Scope::Frame)
		zoom_.clear();
	hover_.clear();
	segSel_.clear();
	invalidate(true);
	emit changed();
}

void Session::nextFrame() { setFrame(frame_ + 1); }
void Session::prevFrame() { setFrame(frame_ - 1); }

void Session::setRange(std::int64_t a, std::int64_t b)
{
	range_ = { std::min(a, b), std::max(a, b) };
	if (rangeFrames().empty()) {
		clearRange();
		return;
	}
	scope_ = Scope::Selection;
	zoom_.clear();
	hover_.clear();
	invalidate(true);
	emit changed();
}

void Session::clearRange()
{
	range_.reset();
	if (scope_ == Scope::Selection)
		scope_ = Scope::Frame;
	zoom_.clear();
	invalidate(true);
	emit changed();
}

void Session::followWait(const Event *e)
{
	if (e && (e->cause == Cause::Runnable || e->cause == Cause::Open))
		return;
	wait_ = e == wait_ ? nullptr : e;
	scope_ = wait_ ? Scope::Wait : Scope::Frame;
	zoom_.clear();
	hover_.clear();
	segSel_.clear();
	invalidate(true);
	emit changed();
}

void Session::setScope(Scope s)
{
	if (s == scope_)
		return;
	if (s == Scope::Selection && !range_)
		return;
	if (s == Scope::Wait && !wait_)
		return;
	scope_ = s;
	zoom_.clear();
	hover_.clear();
	tree_.reset();
	gen_++;
	emit changed();
}

void Session::setZoom(const QStringList &keys)
{
	zoom_ = keys;
	hover_.clear();
	emit changed();
}

void Session::setHover(const QStringList &keys)
{
	if (keys == hover_)
		return;
	hover_ = keys;
	emit changed();
}

void Session::selectSegment(const QString &key)
{
	segSel_ = key == segSel_ ? QString() : key;
	emit changed();
}

void Session::setFollow(bool on)
{
	follow_ = on;
	emit changed();
}

}  // namespace ht
