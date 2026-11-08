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

#include <QCloseEvent>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Window.h"
#include "OAMViewerDialog.h"
#include "ThreeDViewerPage.h"

#include "ViewerTabsDialog.h"

ViewerTabsDialog* ViewerTabsDialog::currentDlg = nullptr;

ViewerTabsDialog::ViewerTabsDialog(MainWindow* parent) : QDialog(parent), mainWindow(parent)
{
    setWindowTitle("Viewers");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1100, 640);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    tabs = new QTabWidget(this);
    layout->addWidget(tabs);

    auto* oam = new OAMViewerDialog(parent);
    oam->setWindowFlags(Qt::Widget);
    tabs->addTab(oam, "OAM");

    auto* threeD = new ThreeDViewerPage(parent);
    tabs->addTab(threeD, "3D");
}

ViewerTabsDialog::~ViewerTabsDialog()
{
    if (currentDlg == this)
        currentDlg = nullptr;
}

ViewerTabsDialog* ViewerTabsDialog::openDlg(MainWindow* parent)
{
    if (currentDlg)
    {
        currentDlg->activateWindow();
        return currentDlg;
    }

    currentDlg = new ViewerTabsDialog(parent);
    currentDlg->show();
    return currentDlg;
}

void ViewerTabsDialog::showTab(int index)
{
    if (tabs)
        tabs->setCurrentIndex(index);
}

void ViewerTabsDialog::closeEvent(QCloseEvent* event)
{
    if (currentDlg == this)
        currentDlg = nullptr;
    QDialog::closeEvent(event);
}
