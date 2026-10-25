// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GoalReplView.h"

#include "QtHost.h"

#include "pcsx2/Host.h"
#include "pcsx2/Jak/GoalRepl.h"

#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QVBoxLayout>

static constexpr qsizetype MAX_HISTORY = 200;

GoalReplView::GoalReplView(const DebuggerViewParameters& parameters)
	: DebuggerView(parameters, MONOSPACE_FONT)
	, m_output(new QPlainTextEdit(this))
	, m_input(new QLineEdit(this))
{
	QVBoxLayout* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(m_output);
	layout->addWidget(m_input);

	m_output->setReadOnly(true);
	m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_output->setMaximumBlockCount(20000);
	m_input->setPlaceholderText(tr("(inspect *target*), dump *target*, help ..."));
	m_input->installEventFilter(this);

	m_history = QString::fromStdString(Host::GetBaseStringSettingValue("JakDebug", "ReplHistory")).split('\x1f', Qt::SkipEmptyParts);
	m_history_position = m_history.size();

	connect(m_input, &QLineEdit::returnPressed, this, &GoalReplView::submit);

	appendOutput(tr("GOAL REPL - type 'help' for commands. Load types with: types <path to all-types.gc>\n"));
}

bool GoalReplView::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == m_input && event->type() == QEvent::KeyPress)
	{
		const int key = static_cast<QKeyEvent*>(event)->key();
		if (key == Qt::Key_Up && m_history_position > 0)
		{
			m_input->setText(m_history[--m_history_position]);
			return true;
		}
		if (key == Qt::Key_Down)
		{
			if (m_history_position + 1 < m_history.size())
				m_input->setText(m_history[++m_history_position]);
			else
			{
				m_history_position = m_history.size();
				m_input->clear();
			}
			return true;
		}
	}
	return DebuggerView::eventFilter(watched, event);
}

void GoalReplView::submit()
{
	const QString line = m_input->text().trimmed();
	if (line.isEmpty() || m_busy)
		return;

	if (m_history.isEmpty() || m_history.last() != line)
	{
		m_history.append(line);
		while (m_history.size() > MAX_HISTORY)
			m_history.removeFirst();
		Host::SetBaseStringSettingValue("JakDebug", "ReplHistory", m_history.join('\x1f').toStdString().c_str());
		Host::CommitBaseSettingChanges();
	}
	m_history_position = m_history.size();
	m_input->clear();

	if (line == QStringLiteral("clear"))
	{
		m_output->clear();
		return;
	}

	appendOutput(QStringLiteral("> %1\n").arg(line));
	m_busy = true;
	m_input->setEnabled(false);

	// commands run on the cpu thread, at the next vsync if the game is running
	QPointer<GoalReplView> self(this);
	Host::RunOnCPUThread([self, command = line.toStdString()]() {
		const std::string result = Goal::Repl::Execute(command);
		QtHost::RunOnUIThread([self, result]() {
			if (!self)
				return;
			self->appendOutput(QString::fromStdString(result));
			self->m_busy = false;
			self->m_input->setEnabled(true);
			self->m_input->setFocus();
		});
	});
}

void GoalReplView::appendOutput(const QString& text)
{
	QTextCursor cursor = m_output->textCursor();
	cursor.movePosition(QTextCursor::End);
	cursor.insertText(text);
	m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}
