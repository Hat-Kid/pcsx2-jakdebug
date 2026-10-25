// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DebuggerView.h"

#include <QtCore/QStringList>

class QLineEdit;
class QPlainTextEdit;

// Front end for Goal::Repl, see pcsx2/Jak/GoalRepl.h.
class GoalReplView final : public DebuggerView
{
	Q_OBJECT

public:
	GoalReplView(const DebuggerViewParameters& parameters);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void submit();
	void appendOutput(const QString& text);

	QPlainTextEdit* m_output;
	QLineEdit* m_input;
	QStringList m_history;
	qsizetype m_history_position = 0;
	bool m_busy = false;
};
