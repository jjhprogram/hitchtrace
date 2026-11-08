// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// The lab's cases: for each, what it simulates, what hitchtrace should show,
// its settings, and how to run it on this machine (the game, the programs
// around it, limits and pinning). The same set-ups as cases/run.sh.
#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ht {

struct Knob {
	QString key, label, suffix, help;
	int value = 0, min = 0, max = 0;
	bool toggle = false;	// shown as a checkbox; value is 0 or 1
};

struct LabCase {
	QString key, title;
	QString what;		// what it simulates
	QString look;		// what to look for in hitchtracegui
	QString warning;	// shown before starting, if any
	QList<Knob> knobs;
};

QList<LabCase> labCases();

// The machine the lab runs on.
struct LabEnv {
	QString self;		// this executable
	QString slackLib;	// libhitchlab_slack.so
	QString dataDir;	// asset pack, scratch file, sockets
	QString gameCpus;	// e.g. "0-3"
	int cpus = 1;		// online CPUs
};

// One process of a case.
struct LabProc {
	QString name;		// hitchlab --run NAME
	QStringList args;
	QString cpus;		// pinned to these CPUs; empty for no pinning
	QStringList wrap;	// run through this first (systemd-run ... for a cgroup)
	QStringList env;	// extra KEY=VALUE
	bool beforeGame = false;	// started before the game, which waits for readyText
	QString readyText;	// in its output when it is ready
	bool wantsGamePid = false;	// gets "-P <the game's process id>"
};

struct LabPlan {
	LabProc game;
	QList<LabProc> helpers;
	QList<QStringList> setup;	// commands run once before anything starts
	int stopAfter = 0;		// seconds; 0 for until stopped
	bool needsPack = false;		// the 256 MB asset pack
	bool needsScratch = false;	// the 1 GB file another program reads
	QString problem;		// why the case cannot run here
};

LabPlan planFor(const LabCase &c, const QMap<QString, int> &values, const LabEnv &env);
QString packPath(const LabEnv &env);
QString scratchPath(const LabEnv &env);
QList<int> parseCpus(const QString &list);	// "0-3,6" -> 0 1 2 3 6; empty if invalid

}  // namespace ht
