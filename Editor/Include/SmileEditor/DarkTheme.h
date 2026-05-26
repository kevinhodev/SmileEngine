#pragma once

#include <QString>
#include <QStringList>

class QApplication;

namespace SmileEditor {
	void ApplyDarkTheme(QApplication& App);
	QString GetStylesDirectoryPath();
	QStringList GetStylesheetFiles();
	void LoadAndApplyStylesheets(QApplication& App);
} 

