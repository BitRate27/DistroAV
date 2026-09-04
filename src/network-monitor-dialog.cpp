/******************************************************************************
	Copyright (C) 2016-2026 DistroAV <contact@distroav.org>

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, see <https://www.gnu.org/licenses/>.
******************************************************************************/

#include "network-monitor-dialog.h"
#include "network-monitor.h"
#include "ndi-adapter-table-widget.h"
#include "ndi-sender-table-widget.h"
#include "ndi-receiver-table-widget.h"
#include "ndi-config-widget.h"

#include <obs-frontend-api.h>

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QPushButton>
#include <QCheckBox>
#include <QSizePolicy>
#include <QWidget>
#include <QTabWidget>
#include <QPointer>
#include <QClipboard>
#include <QGuiApplication>

// This is used as a global by ndi-source.cpp / plugin-main.h; declared extern there.
extern NetworkMonitor *network_monitor;
static QPointer<QDialog> s_dialog;
bool open_network_monitor_dialog()
{
	if (s_dialog) {
		s_dialog->show();
		s_dialog->raise();
		s_dialog->activateWindow();
		return true;
	}

	// Get the NdiNetworkMonitor singleton instance and get the current network report.
	NdiNetworkReport report = network_monitor->getNetworkReport();

	// non-blocking dialog that will be deleted when closed
	QDialog *dialog = new QDialog();
	s_dialog = dialog;
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle("Network Monitor");
	dialog->setWindowModality(Qt::NonModal);
	dialog->setMinimumSize(400, 100);
	dialog->setSizeGripEnabled(true);
	QObject::connect(dialog, &QObject::destroyed, []() { s_dialog = nullptr; });

	QVBoxLayout *layout = new QVBoxLayout(dialog);

	QTabWidget *tabWidget = new QTabWidget(dialog);

	NdiAdapterTableWidget *adapterTable = new NdiAdapterTableWidget(tabWidget);
	adapterTable->setAdapters(report.adapters);
	tabWidget->addTab(adapterTable, dialog->tr("Adapters"));

	NdiSenderTableWidget *senderTable = new NdiSenderTableWidget(tabWidget);
	senderTable->setSenders(network_monitor->getAllSenderInfo());
	tabWidget->addTab(senderTable, dialog->tr("Senders"));

	NdiReceiverTableWidget *receiverTable = new NdiReceiverTableWidget(tabWidget);
	receiverTable->setReceivers(network_monitor->getAllReceiverInfo());
	tabWidget->addTab(receiverTable, dialog->tr("Receivers"));

	QString configPath;
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
	const QString home = qEnvironmentVariableIsSet("HOME") ? QString::fromLocal8Bit(qgetenv("HOME"))
							       : QDir::homePath();
	configPath = home + "/.ndi/ndi-config.v1.json";
#else
	configPath = "C:\\ProgramData\\NDI\\ndi-config.v1.json";
#endif
	NdiNetworkConfigWidget *configWidget = new NdiNetworkConfigWidget(configPath, tabWidget);
	tabWidget->addTab(configWidget, dialog->tr("Config"));

	layout->addWidget(tabWidget);

	// Footer with OK buttons. OK closes the top-level window (dialog).
	QHBoxLayout *footer = new QHBoxLayout();
	footer->setContentsMargins(0, 6, 0, 0);
	footer->setSpacing(8);
	footer->addStretch();

	QPushButton *helpBtn = new QPushButton(dialog->tr("Help"), dialog);
	helpBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	footer->addWidget(helpBtn);
	QObject::connect(helpBtn, &QPushButton::clicked, dialog, []() {
		QDesktopServices::openUrl(
			QUrl("https://github.com/DistroAV/DistroAV/wiki/Network-Monitor-Documentation"));
	});
	QPushButton *dumpBtn = new QPushButton(dialog->tr("Dump to OBS Log"), dialog);
	dumpBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	footer->addWidget(dumpBtn);
	QObject::connect(dumpBtn, &QPushButton::clicked, dialog, [configWidget]() {
		network_monitor->dumpNetworkReportToLog();
		configWidget->dumpConfigToLog();
	});
	QPushButton *copyBtn = new QPushButton(dialog->tr("Copy"), dialog);
	copyBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	footer->addWidget(copyBtn);
	QObject::connect(copyBtn, &QPushButton::clicked, dialog,
			 [tabWidget, adapterTable, senderTable, receiverTable, configWidget]() {
				 QWidget *current = tabWidget->currentWidget();
				 QString text;
				 if (current == adapterTable) {
					 text = QString::fromUtf8(network_monitor->getFormattedAdapterReport());
				 } else if (current == senderTable) {
					 text = QString::fromUtf8(network_monitor->getFormattedSenderReport());
				 } else if (current == receiverTable) {
					 text = QString::fromUtf8(network_monitor->getFormattedReceiverReport());
				 } else if (current == configWidget) {
					 text = configWidget->getFormattedConfigText();
				 }
				 QGuiApplication::clipboard()->setText(text);
			 });

	QPushButton *okBtn = new QPushButton(dialog->tr("Close"), dialog);
	okBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	footer->addWidget(okBtn);
	// OK button closes the parent dialog/window
	QObject::connect(okBtn, &QPushButton::clicked, dialog, [dialog]() {
		QWidget *w = dialog->window();
		if (w)
			w->close();
	});
	footer->setAlignment(Qt::AlignRight);
	layout->addLayout(footer);
	layout->setAlignment(footer, Qt::AlignRight);

	// show() returns immediately properties dialog and OBS remain interactive
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
	return true;
}

void close_network_monitor_dialog()
{
	if (s_dialog) {
		s_dialog->close();
	}
}
