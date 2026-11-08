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

#ifndef THREEDVIEWERPAGE_H
#define THREEDVIEWERPAGE_H

#include <QWidget>
#include <QImage>
#include <QTimer>

#include <vector>

#include "types.h"

class EmuInstance;
class MainWindow;
class QCheckBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class ThreeDCanvas;

class ThreeDViewerPage : public QWidget
{
    Q_OBJECT

public:
    struct VertexView
    {
        int x = 0, y = 0, z = 0, w = 0;
        int r = 0, g = 0, b = 0;
        int u = 0, v = 0;
    };

    struct PolyView
    {
        melonDS::u32 attr = 0;
        melonDS::u32 texParam = 0;
        melonDS::u32 texPalette = 0;
        int type = 0;
        int numVertices = 0;
        bool translucent = false;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        VertexView verts[10];
    };

    struct Snapshot
    {
        bool valid = false;
        std::vector<PolyView> polygons;

        melonDS::u32 dispCnt = 0;
        melonDS::u32 gxStat = 0;
        int fifoCmd = 0, fifoPipe = 0, fifoStall = 0;
        int matrixMode = 0;
        int viewport[6] = {};
        int proj[16] = {};
        int pos[16] = {};
        int vec[16] = {};
        int tex[16] = {};
        melonDS::u32 fogColor = 0;
        melonDS::u32 fogOffset = 0;
        int numVertices = 0;
        int numPolygons = 0;
        int renderPolygons = 0;

        QImage frame;
        QImage texture;
        int texWidth = 0;
        int texHeight = 0;
    };

    explicit ThreeDViewerPage(MainWindow* parent);
    ~ThreeDViewerPage();

    void refresh();
    void setFrozen(bool frozen);

    const Snapshot& snapshot() const { return snap; }
    int selected() const { return selectedIndex; }
    void selectPolygon(int index);

private:
    void updateDetails();
    void updateTexture();

    EmuInstance* emuInstance = nullptr;
    QCheckBox* freezeBox = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* texPreview = nullptr;
    ThreeDCanvas* canvas = nullptr;
    QListWidget* polyList = nullptr;
    QPlainTextEdit* details = nullptr;
    QTimer* timer = nullptr;

    Snapshot snap;
    int selectedIndex = 0;
    bool pausedByUs = false;
};

#endif // THREEDVIEWERPAGE_H
