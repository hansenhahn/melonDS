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

#ifndef OAMVIEWERDIALOG_H
#define OAMVIEWERDIALOG_H

#include <QDialog>
#include <QImage>
#include <QTimer>

#include <array>

#include "types.h"

class EmuInstance;
class QCloseEvent;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;

struct OAMEntry
{
    bool used = false;
    bool affine = false;
    bool disabled = false;
    bool bitmap = false;
    bool mosaic = false;
    bool bpp8 = false;
    int shape = 0;
    int size = 0;
    int x = 0;
    int y = 0;
    int tile = 0;
    int priority = 0;
    int palette = 0;
    int mode = 0;
    int width = 0;
    int height = 0;
    int rsGroup = -1;
    int pa = 0;
    int pb = 0;
    int pc = 0;
    int pd = 0;
    melonDS::u16 attr0 = 0;
    melonDS::u16 attr1 = 0;
    melonDS::u16 attr2 = 0;
    melonDS::u32 tileAddr = 0;
    QImage image;
};

class OAMGridWidget;
class OAMPreviewWidget;

class OAMViewerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OAMViewerDialog(QWidget* parent);
    ~OAMViewerDialog();

    static OAMViewerDialog* currentDlg;
    static OAMViewerDialog* openDlg(QWidget* parent)
    {
        if (currentDlg)
        {
            currentDlg->activateWindow();
            return currentDlg;
        }

        currentDlg = new OAMViewerDialog(parent);
        currentDlg->show();
        return currentDlg;
    }

    const OAMEntry& entry(int index) const { return entries[index]; }
    int selected() const { return selectedIndex; }

    void selectEntry(int index);
    void refresh();
    void setFrozen(bool frozen);

    std::array<OAMEntry, 128> entries;

private:
    void closeEvent(QCloseEvent* event) override;
    void updateDetails();

    EmuInstance* emuInstance = nullptr;
    QComboBox* engineBox = nullptr;
    QCheckBox* freezeBox = nullptr;
    OAMGridWidget* grid = nullptr;
    OAMPreviewWidget* preview = nullptr;
    QPlainTextEdit* details = nullptr;
    QLabel* statusLabel = nullptr;
    QTimer* timer = nullptr;
    int selectedIndex = 0;
    bool pausedByUs = false;
};

#endif // OAMVIEWERDIALOG_H
