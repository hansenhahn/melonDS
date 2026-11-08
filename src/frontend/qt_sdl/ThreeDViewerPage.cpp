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

#include <QCheckBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "EmuInstance.h"
#include "EmuThread.h"
#include "GPU.h"
#include "GPU3D_Texcache.h"
#include "NDS.h"
#include "main.h"

#include "ThreeDViewerPage.h"

using namespace melonDS;

namespace
{

QString hexv(u32 v, int digits)
{
    return QString("%1").arg(v, digits, 16, QChar('0'));
}

QImage DecodeTexture(NDS* nds, u32 texParam, u32 texPalette)
{
    const u32 fmt = (texParam >> 26) & 0x7;
    if (!nds || fmt == 0) return QImage();

    GPU& gpu = nds->GPU;
    auto texDirty = gpu.VRAMDirty_Texture.DeriveState(gpu.VRAMMap_Texture, gpu);
    auto palDirty = gpu.VRAMDirty_TexPal.DeriveState(gpu.VRAMMap_TexPal, gpu);
    gpu.MakeVRAMFlat_TextureCoherent(texDirty);
    gpu.MakeVRAMFlat_TexPalCoherent(palDirty);

    const u32 w = TextureWidth(texParam);
    const u32 h = TextureHeight(texParam);
    const u32 addr = (texParam & 0xFFFF) * 8;
    std::vector<u32> buf((size_t)w * h, 0);

    if (fmt == 7)
    {
        ConvertBitmapTexture<outputFmt_RGB6A5>(w, h, buf.data(), addr, gpu);
    }
    else if (fmt == 5)
    {
        u32 slot1 = 0x20000 + ((addr & 0x1FFFC) >> 1);
        if (addr >= 0x40000) slot1 += 0x10000;
        ConvertCompressedTexture<outputFmt_RGB6A5>(w, h, buf.data(), addr, slot1,
                                                   texPalette * 16, gpu);
    }
    else
    {
        u32 palAddr = texPalette * 16;
        if (fmt == 2) palAddr >>= 1;
        palAddr &= 0x1FFFF;
        const bool color0Transparent = (texParam & (1u << 29)) != 0;

        switch (fmt)
        {
        case 1: ConvertAXIYTexture<outputFmt_RGB6A5, 3, 5>(w, h, buf.data(), addr, palAddr, gpu); break;
        case 6: ConvertAXIYTexture<outputFmt_RGB6A5, 5, 3>(w, h, buf.data(), addr, palAddr, gpu); break;
        case 2: ConvertNColorsTexture<outputFmt_RGB6A5, 2>(w, h, buf.data(), addr, palAddr, color0Transparent, gpu); break;
        case 3: ConvertNColorsTexture<outputFmt_RGB6A5, 4>(w, h, buf.data(), addr, palAddr, color0Transparent, gpu); break;
        case 4: ConvertNColorsTexture<outputFmt_RGB6A5, 8>(w, h, buf.data(), addr, palAddr, color0Transparent, gpu); break;
        default: return QImage();
        }
    }

    QImage img((int)w, (int)h, QImage::Format_ARGB32);
    for (u32 y = 0; y < h; y++)
    {
        for (u32 x = 0; x < w; x++)
        {
            const u32 px = buf[(size_t)y * w + x];
            const int r = (int)(px & 0xFF) * 255 / 63;
            const int g = (int)((px >> 8) & 0xFF) * 255 / 63;
            const int b = (int)((px >> 16) & 0xFF) * 255 / 63;
            const int a = (int)((px >> 24) & 0x1F) * 255 / 31;
            img.setPixel((int)x, (int)y, qRgba(r, g, b, a));
        }
    }
    return img;
}

}

class ThreeDCanvas : public QWidget
{
public:
    explicit ThreeDCanvas(ThreeDViewerPage* owner) : QWidget(owner), page(owner) {}

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);

        const auto& s = page->snapshot();
        if (!s.valid)
        {
            p.setPen(Qt::white);
            p.drawText(rect(), Qt::AlignCenter, "No emulator / no 3D");
            return;
        }

        int scale = std::min(width() / 256, height() / 192);
        if (scale < 1) scale = 1;
        const int w = 256 * scale;
        const int h = 192 * scale;
        const int ox = (width() - w) / 2;
        const int oy = (height() - h) / 2;

        if (!s.frame.isNull())
            p.drawImage(QRect(ox, oy, w, h), s.frame);

        for (int i = 0; i < (int)s.polygons.size(); i++)
        {
            const auto& poly = s.polygons[i];
            const bool sel = (i == page->selected());
            const int x0 = ox + poly.x0 * scale;
            const int y0 = oy + poly.y0 * scale;
            const int x1 = ox + poly.x1 * scale;
            const int y1 = oy + poly.y1 * scale;

            if (sel)
            {
                p.fillRect(QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1),
                           QColor(255, 0, 0, 60));
                p.setPen(QPen(Qt::red, 2));
                p.drawRect(QRect(x0, y0, x1 - x0, y1 - y0));
                p.setPen(Qt::yellow);
                p.drawText(x0 + 2, y0 + 12, QString("#%1").arg(i));
            }
            else
            {
                p.setPen(QPen(QColor(0, 255, 0, 160), 1));
                p.drawRect(QRect(x0, y0, x1 - x0, y1 - y0));
            }
        }
    }

private:
    ThreeDViewerPage* page;
};

ThreeDViewerPage::ThreeDViewerPage(MainWindow* parent) : QWidget(parent)
{
    emuInstance = parent->getEmuInstance();

    auto* layout = new QHBoxLayout(this);

    auto* left = new QVBoxLayout();
    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel("3D (engine A):"));
    freezeBox = new QCheckBox("Freeze");
    freezeBox->setToolTip("Pause the emulator while inspecting the 3D frame");
    top->addWidget(freezeBox);
    top->addStretch();
    statusLabel = new QLabel();
    top->addWidget(statusLabel);
    left->addLayout(top);

    canvas = new ThreeDCanvas(this);
    canvas->setMinimumSize(256, 192);
    left->addWidget(canvas, 1);
    layout->addLayout(left, 2);

    auto* right = new QVBoxLayout();
    polyList = new QListWidget();
    polyList->setMinimumWidth(260);
    right->addWidget(polyList, 1);

    texPreview = new QLabel();
    texPreview->setMinimumHeight(160);
    texPreview->setAlignment(Qt::AlignCenter);
    texPreview->setText("no texture");
    right->addWidget(texPreview);

    details = new QPlainTextEdit();
    details->setReadOnly(true);
    details->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    details->setMinimumWidth(320);
    details->setMaximumWidth(420);
    right->addWidget(details, 2);
    layout->addLayout(right, 1);

    connect(freezeBox, &QCheckBox::toggled, this, &ThreeDViewerPage::setFrozen);
    connect(polyList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        selectedIndex = row;
        canvas->update();
        updateDetails();
    });

    timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, &ThreeDViewerPage::refresh);
    timer->start();

    refresh();
}

ThreeDViewerPage::~ThreeDViewerPage()
{
    if (pausedByUs)
        emuInstance->getEmuThread()->emuUnpause(false);
}

void ThreeDViewerPage::setFrozen(bool frozen)
{
    if (frozen && !pausedByUs)
    {
        emuInstance->getEmuThread()->emuPause(false);
        pausedByUs = true;
    }
    else if (!frozen && pausedByUs)
    {
        emuInstance->getEmuThread()->emuUnpause(false);
        pausedByUs = false;
    }
    refresh();
}

void ThreeDViewerPage::selectPolygon(int index)
{
    if (snap.polygons.empty()) { selectedIndex = 0; return; }
    selectedIndex = std::clamp(index, 0, (int)snap.polygons.size() - 1);
    QSignalBlocker block(polyList);
    polyList->setCurrentRow(selectedIndex);
    canvas->update();
    updateTexture();
    updateDetails();
}

void ThreeDViewerPage::refresh()
{
    NDS* nds = emuInstance->getNDS();
    snap = Snapshot{};

    if (!nds)
    {
        statusLabel->setText("No emulator");
        polyList->clear();
        canvas->update();
        updateDetails();
        return;
    }

    GPU3D& g = nds->GPU.GPU3D;
    snap.valid = true;
    snap.dispCnt = g.DispCnt;
    snap.gxStat = g.GXStat;
    snap.fifoCmd = g.CmdFIFO.Level();
    snap.fifoPipe = g.CmdPIPE.Level();
    snap.fifoStall = g.CmdStallQueue.Level();
    snap.matrixMode = g.MatrixMode;
    for (int i = 0; i < 6; i++) snap.viewport[i] = g.Viewport[i];
    for (int i = 0; i < 16; i++)
    {
        snap.proj[i] = g.ProjMatrix[i];
        snap.pos[i] = g.PosMatrix[i];
        snap.vec[i] = g.VecMatrix[i];
        snap.tex[i] = g.TexMatrix[i];
    }
    snap.fogColor = g.FogColor;
    snap.fogOffset = g.FogOffset;
    snap.numVertices = g.NumVertices;
    snap.numPolygons = g.NumPolygons;
    snap.renderPolygons = g.RenderNumPolygons;

    u32 count = g.RenderNumPolygons;
    const u32 maxPolys = (u32)g.RenderPolygonRAM.size();
    if (count > maxPolys) count = maxPolys;
    snap.polygons.reserve(count);
    for (u32 i = 0; i < count; i++)
    {
        const Polygon* p = g.RenderPolygonRAM[i];
        if (!p) continue;

        PolyView pv;
        pv.attr = p->Attr;
        pv.texParam = p->TexParam;
        pv.texPalette = p->TexPalette;
        pv.type = p->Type;
        pv.numVertices = std::min<u32>(p->NumVertices, 10);
        pv.translucent = p->Translucent;

        int minx = 0x7FFFFFFF, miny = 0x7FFFFFFF;
        int maxx = -0x7FFFFFFF, maxy = -0x7FFFFFFF;
        for (int v = 0; v < pv.numVertices; v++)
        {
            const Vertex* vv = p->Vertices[v];
            if (!vv) continue;
            VertexView& out = pv.verts[v];
            out.x = vv->FinalPosition[0];
            out.y = vv->FinalPosition[1];
            out.z = p->FinalZ[v];
            out.w = p->FinalW[v];
            out.r = vv->FinalColor[0] >> 12;
            out.g = vv->FinalColor[1] >> 12;
            out.b = vv->FinalColor[2] >> 12;
            out.u = vv->TexCoords[0];
            out.v = vv->TexCoords[1];
            minx = std::min(minx, out.x);
            miny = std::min(miny, out.y);
            maxx = std::max(maxx, out.x);
            maxy = std::max(maxy, out.y);
        }
        if (pv.numVertices == 0) { minx = miny = maxx = maxy = 0; }
        pv.x0 = std::clamp(minx, 0, 255);
        pv.y0 = std::clamp(miny, 0, 191);
        pv.x1 = std::clamp(maxx, 0, 255);
        pv.y1 = std::clamp(maxy, 0, 191);
        snap.polygons.push_back(pv);
    }

    void* top = nullptr;
    void* bottom = nullptr;
    if (nds->GPU.GetFramebuffers(&top, &bottom) && top)
    {
        snap.frame = QImage((const uchar*)top, 256, 192, 256 * 4,
                            QImage::Format_ARGB32).copy();
    }

    if (polyList->count() != (int)snap.polygons.size())
    {
        QSignalBlocker block(polyList);
        polyList->clear();
        for (int i = 0; i < (int)snap.polygons.size(); i++)
        {
            const auto& p = snap.polygons[i];
            polyList->addItem(QString("#%1 type=%2 verts=%3 %4x%5%6")
                .arg(i).arg(p.type).arg(p.numVertices)
                .arg(p.x1 - p.x0 + 1).arg(p.y1 - p.y0 + 1)
                .arg(p.translucent ? " T" : ""));
        }
        polyList->setCurrentRow(std::min(selectedIndex, (int)snap.polygons.size() - 1));
    }

    statusLabel->setText(QString("polys %1/%2  verts %3  FIFO %4/%5/%6")
        .arg(snap.renderPolygons).arg(snap.numPolygons).arg(snap.numVertices)
        .arg(snap.fifoCmd).arg(snap.fifoPipe).arg(snap.fifoStall));

    canvas->update();
    updateTexture();
    updateDetails();
}

void ThreeDViewerPage::updateTexture()
{
    snap.texture = QImage();
    snap.texWidth = 0;
    snap.texHeight = 0;

    if (!snap.valid || snap.polygons.empty())
    {
        texPreview->setText("no texture");
        return;
    }

    const int idx = std::clamp(selectedIndex, 0, (int)snap.polygons.size() - 1);
    const auto& p = snap.polygons[idx];
    if (((p.texParam >> 26) & 0x7) == 0)
    {
        texPreview->setText("no texture");
        return;
    }

    NDS* nds = emuInstance->getNDS();
    snap.texture = DecodeTexture(nds, p.texParam, p.texPalette);
    if (snap.texture.isNull())
    {
        texPreview->setText("texture decode failed");
        return;
    }

    snap.texWidth = snap.texture.width();
    snap.texHeight = snap.texture.height();
    texPreview->setPixmap(QPixmap::fromImage(snap.texture).scaled(
        180, 180, Qt::KeepAspectRatio, Qt::FastTransformation));
}

void ThreeDViewerPage::updateDetails()
{
    QString t;

    if (!snap.valid)
    {
        t = "No emulator";
    }
    else
    {
        t += QString("DISP3DCNT  0x%1\n").arg(hexv(snap.dispCnt, 8));
        t += QString("GXSTAT     0x%1\n").arg(hexv(snap.gxStat, 8));
        t += QString("FIFO       cmd=%1 pipe=%2 stall=%3\n")
                .arg(snap.fifoCmd).arg(snap.fifoPipe).arg(snap.fifoStall);
        t += QString("MatrixMode %1\n").arg(snap.matrixMode);
        t += QString("Viewport   x=%1 y=%2 w=%3 h=%4\n")
                .arg(snap.viewport[0]).arg(snap.viewport[3])
                .arg(snap.viewport[4]).arg(snap.viewport[5]);
        t += QString("Proj        %1 %2 %3 %4\n")
                .arg(snap.proj[0]).arg(snap.proj[5]).arg(snap.proj[10]).arg(snap.proj[15]);
        t += QString("Polygons   render=%1 total=%2\n")
                .arg(snap.renderPolygons).arg(snap.numPolygons);
        t += "\n";

        if (snap.polygons.empty())
        {
            t += "No polygons.\n";
        }
        else
        {
            const int idx = std::clamp(selectedIndex, 0, (int)snap.polygons.size() - 1);
            const auto& p = snap.polygons[idx];
            t += QString("--- Polygon #%1 ---\n").arg(idx);
            t += QString("Attr       0x%1\n").arg(hexv(p.attr, 8));
            t += QString("TexParam   0x%1\n").arg(hexv(p.texParam, 8));
            t += QString("TexPalette 0x%1\n").arg(hexv(p.texPalette, 6));
            t += QString("TexFormat  %1  %2x%3\n")
                    .arg((p.texParam >> 26) & 0x7).arg(snap.texWidth).arg(snap.texHeight);
            t += QString("Type       %1%2\n").arg(p.type).arg(p.translucent ? " (translucent)" : "");
            t += QString("Verts      %1\n").arg(p.numVertices);
            t += QString("BBox       %1,%2 .. %3,%4\n").arg(p.x0).arg(p.y0).arg(p.x1).arg(p.y1);
            t += "\n";
            for (int v = 0; v < p.numVertices; v++)
            {
                const auto& vv = p.verts[v];
                t += QString("v%1 screenpos %2,%3  z=%4 w=%5  rgb=%6,%7,%8  uv=%9,%10\n")
                        .arg(v).arg(vv.x).arg(vv.y).arg(vv.z).arg(vv.w)
                        .arg(vv.r).arg(vv.g).arg(vv.b).arg(vv.u).arg(vv.v);
            }
        }
    }

    if (details->toPlainText() == t)
        return;

    QScrollBar* sb = details->verticalScrollBar();
    const int scroll = sb->value();
    details->setPlainText(t);
    sb->setValue(std::min(scroll, sb->maximum()));
}
