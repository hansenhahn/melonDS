/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef VIEWERTABSDIALOG_H
#define VIEWERTABSDIALOG_H

#include <QDialog>

class MainWindow;
class QCloseEvent;
class QTabWidget;

class ViewerTabsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ViewerTabsDialog(MainWindow* parent);
    ~ViewerTabsDialog();

    static ViewerTabsDialog* currentDlg;
    static ViewerTabsDialog* openDlg(MainWindow* parent);

    void showTab(int index);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    MainWindow* mainWindow = nullptr;
    QTabWidget* tabs = nullptr;
};

#endif // VIEWERTABSDIALOG_H
