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

#include "OAMViewerDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include <cstring>
#include <functional>

#include "EmuInstance.h"
#include "EmuThread.h"
#include "GPU.h"
#include "NDS.h"
#include "main.h"

using namespace melonDS;

OAMViewerDialog* OAMViewerDialog::currentDlg = nullptr;

namespace
{

constexpr int kOAMSize = 0x400;
constexpr int kPaletteOffsetA = 0x200;
constexpr int kPaletteOffsetB = 0x600;
constexpr u32 kObjBaseA = 0x06400000;
constexpr u32 kObjBaseB = 0x06600000;

const int kWidths[3][4] = {
    {8, 16, 32, 64},
    {16, 32, 32, 64},
    {8, 8, 16, 32},
};
const int kHeights[3][4] = {
    {8, 16, 32, 64},
    {8, 8, 16, 32},
    {16, 32, 32, 64},
};

u16 read16(const u8* data, int offset)
{
    u16 value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

void decodeSize(int shape, int size, int& width, int& height)
{
    if (shape < 0 || shape > 2 || size < 0 || size > 3)
    {
        width = 0;
        height = 0;
        return;
    }
    width = kWidths[shape][size];
    height = kHeights[shape][size];
}

u32 tileAddress(const OAMEntry& entry, u32 dispcnt, u32 mask, bool oneD)
{
    const int bytes = entry.bpp8 ? 64 : 32;
    u32 addr;
    if (oneD)
    {
        const u32 boundary = 32u << ((dispcnt >> 20) & 0x3);
        addr = entry.tile * boundary;
    }
    else
    {
        const int maskX = entry.bpp8 ? 0x0F : 0x1F;
        const int shift = entry.bpp8 ? 4 : 5;
        addr = u32(entry.tile & maskX) * u32(bytes) + u32(entry.tile >> shift) * 0x400u;
    }
    return addr & mask;
}

QImage renderSprite(const OAMEntry& entry, const u8* vram, u32 mask, const u8* palette, bool oneD)
{
    if (!entry.used || entry.bitmap || entry.width <= 0 || entry.height <= 0)
        return QImage();

    QImage image(entry.width, entry.height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    const int bytes = entry.bpp8 ? 64 : 32;
    const int tilesPerRow = entry.width / 8;
    const u16* pal = reinterpret_cast<const u16*>(palette);

    for (int tileY = 0; tileY < entry.height / 8; tileY++)
    {
        for (int tileX = 0; tileX < tilesPerRow; tileX++)
        {
            const u32 tile = entry.tileAddr
                + (oneD ? u32(tileY * tilesPerRow + tileX) * bytes
                        : u32(tileX) * bytes + u32(tileY) * 0x400u);

            for (int py = 0; py < 8; py++)
            {
                for (int px = 0; px < 8; px++)
                {
                    const u32 offset = tile + u32(py * (entry.bpp8 ? 8 : 4))
                        + u32(entry.bpp8 ? px : px / 2);
                    const u8 value = vram[offset & mask];
                    const int index = entry.bpp8
                        ? value
                        : ((px & 1) ? (value >> 4) : (value & 0xF));
                    const int palIndex = entry.bpp8 ? index : entry.palette * 16 + index;
                    if (palIndex == 0)
                        continue;
                    const u16 color = pal[palIndex & 0xFF];
                    const int r = (color & 0x1F) << 3;
                    const int g = ((color >> 5) & 0x1F) << 3;
                    const int b = ((color >> 10) & 0x1F) << 3;
                    image.setPixel(tileX * 8 + px, tileY * 8 + py, qRgb(r, g, b));
                }
            }
        }
    }

    return image;
}

QString modeName(int mode)
{
    switch (mode)
    {
    case 0: return "Normal";
    case 1: return "Semi-transparent";
    case 2: return "OBJ window";
    case 3: return "Bitmap";
    default: return "?";
    }
}

QString typeName(const OAMEntry& entry)
{
    if (entry.disabled) return "Disabled";
    if (entry.bitmap) return "Bitmap";
    if (entry.affine) return (entry.attr0 & (1 << 9)) ? "Affine double-size" : "Affine";
    return "Normal";
}

QString hex(u32 value, int digits)
{
    return QString("%1").arg(value, digits, 16, QChar('0')).toUpper();
}

}

class OAMGridWidget : public QWidget
{
public:
    explicit OAMGridWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(16 * 36, 8 * 36);
    }

    std::function<void(int)> onSelected;
    OAMViewerDialog* dialog = nullptr;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(24, 24, 24));

        const double cellW = width() / 16.0;
        const double cellH = height() / 8.0;
        for (int index = 0; index < 128; index++)
        {
            const int col = index % 16;
            const int row = index / 16;
            const QRectF cell(col * cellW, row * cellH, cellW, cellH);
            const QRectF inner = cell.adjusted(1, 1, -1, -1);
            painter.fillRect(inner, QColor(34, 34, 34));

            const OAMEntry& entry = dialog->entry(index);
            if (entry.used)
            {
                if (!entry.image.isNull())
                {
                    const QImage scaled = entry.image.scaled(
                        inner.size().toSize(), Qt::KeepAspectRatio, Qt::FastTransformation);
                    painter.drawImage(
                        inner.topLeft() + QPointF(
                            (inner.width() - scaled.width()) / 2.0,
                            (inner.height() - scaled.height()) / 2.0),
                        scaled);
                }
                else
                {
                    painter.setPen(QColor(180, 180, 180));
                    painter.drawText(inner, Qt::AlignCenter, entry.bitmap ? "BM" : "?");
                }
            }
            else
            {
                painter.setPen(QPen(QColor(190, 40, 40), 2));
                painter.drawLine(inner.topLeft(), inner.bottomRight());
            }

            painter.setPen(index == dialog->selected() ? QColor(255, 220, 80) : QColor(60, 60, 60));
            painter.drawRect(inner);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint pos = event->position().toPoint();
#else
        const QPoint pos = event->pos();
#endif
        const double cellW = width() / 16.0;
        const double cellH = height() / 8.0;
        const int col = int(pos.x() / cellW);
        const int row = int(pos.y() / cellH);
        if (col < 0 || col >= 16 || row < 0 || row >= 8)
            return;
        if (onSelected)
            onSelected(row * 16 + col);
    }
};

class OAMPreviewWidget : public QWidget
{
public:
    explicit OAMPreviewWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(240, 240);
    }

    void setEntry(const OAMEntry& value, int valueIndex)
    {
        entry = value;
        index = valueIndex;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(28, 28, 28));

        if (!entry.used || entry.image.isNull())
        {
            painter.setPen(QColor(180, 180, 180));
            painter.drawText(rect(), Qt::AlignCenter,
                             entry.bitmap ? "Bitmap OBJ (not rendered)" : "No sprite");
            return;
        }

        const int availableW = width() - 20;
        const int availableH = height() - 40;
        const int scale = qMax(1, qMin(availableW / entry.width, availableH / entry.height));
        const QImage scaled = entry.image.scaled(
            entry.width * scale, entry.height * scale,
            Qt::KeepAspectRatio, Qt::FastTransformation);
        const int x = (width() - scaled.width()) / 2;
        const int y = 20 + (availableH - scaled.height()) / 2;

        painter.drawImage(x, y, scaled);
        painter.setPen(QPen(Qt::white, 1, Qt::DashLine));
        painter.drawRect(x, y, scaled.width(), scaled.height());

        if (entry.affine && (entry.attr0 & (1 << 9)))
        {
            const int dw = scaled.width() * 2;
            const int dh = scaled.height() * 2;
            painter.setPen(QPen(QColor(255, 220, 80), 1, Qt::DashLine));
            painter.drawRect(x - (dw - scaled.width()) / 2,
                             y - (dh - scaled.height()) / 2, dw, dh);
        }

        painter.setPen(QColor(220, 220, 220));
        painter.drawText(8, 14, QString("%1x%2  OAM %3")
                                    .arg(entry.width)
                                    .arg(entry.height)
                                    .arg(index, 3, 10, QChar('0')));
    }

private:
    OAMEntry entry;
    int index = 0;
};

OAMViewerDialog::OAMViewerDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("OAM Viewer");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(980, 580);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto* layout = new QHBoxLayout(this);

    auto* left = new QVBoxLayout();
    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel("Engine:"));
    engineBox = new QComboBox();
    engineBox->addItem("Engine A");
    engineBox->addItem("Engine B");
    top->addWidget(engineBox);
    freezeBox = new QCheckBox("Freeze");
    freezeBox->setToolTip("Pause the emulator while inspecting OAM/VRAM");
    top->addWidget(freezeBox);
    top->addStretch();
    statusLabel = new QLabel();
    top->addWidget(statusLabel);
    left->addLayout(top);

    grid = new OAMGridWidget(this);
    grid->dialog = this;
    grid->onSelected = [this](int index) { selectEntry(index); };
    left->addWidget(grid, 1);
    layout->addLayout(left, 1);

    auto* right = new QVBoxLayout();
    preview = new OAMPreviewWidget(this);
    right->addWidget(preview);
    details = new QPlainTextEdit();
    details->setReadOnly(true);
    details->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    details->setMinimumWidth(340);
    details->setMaximumWidth(420);
    right->addWidget(details, 1);
    layout->addLayout(right);

    connect(engineBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refresh(); });
    connect(freezeBox, &QCheckBox::toggled, this, &OAMViewerDialog::setFrozen);

    timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, &OAMViewerDialog::refresh);
    timer->start();

    refresh();
}

OAMViewerDialog::~OAMViewerDialog()
{
    if (pausedByUs)
        emuInstance->getEmuThread()->emuUnpause(false);
    if (currentDlg == this)
        currentDlg = nullptr;
}

void OAMViewerDialog::closeEvent(QCloseEvent* event)
{
    timer->stop();
    if (pausedByUs)
    {
        emuInstance->getEmuThread()->emuUnpause(false);
        pausedByUs = false;
    }
    if (currentDlg == this)
        currentDlg = nullptr;
    QDialog::closeEvent(event);
}

void OAMViewerDialog::setFrozen(bool frozen)
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

void OAMViewerDialog::selectEntry(int index)
{
    selectedIndex = qBound(0, index, 127);
    grid->update();
    updateDetails();
}

void OAMViewerDialog::refresh()
{
    NDS* nds = emuInstance->getNDS();
    if (!nds)
    {
        entries.fill(OAMEntry{});
        grid->update();
        updateDetails();
        statusLabel->setText("No emulator");
        return;
    }

    const bool engineB = engineBox->currentIndex() == 1;
    GPU& gpu = nds->GPU;
    const u32 dispcnt = engineB ? gpu.GPU2D_B.DispCnt : gpu.GPU2D_A.DispCnt;
    u8* objvram = nullptr;
    u32 objmask = 0;
    if (engineB)
        gpu.GPU2D_B.GetOBJVRAM(objvram, objmask);
    else
        gpu.GPU2D_A.GetOBJVRAM(objvram, objmask);

    const u8* oam = gpu.OAM + (engineB ? kOAMSize : 0);
    const u8* palette = gpu.Palette + (engineB ? kPaletteOffsetB : kPaletteOffsetA);
    const bool oneD = (dispcnt & (1 << 4)) != 0;

    int used = 0;
    for (int index = 0; index < 128; index++)
    {
        OAMEntry entry;
        const int offset = index * 8;
        entry.attr0 = read16(oam, offset + 0);
        entry.attr1 = read16(oam, offset + 2);
        entry.attr2 = read16(oam, offset + 4);

        entry.y = entry.attr0 & 0xFF;
        entry.x = entry.attr1 & 0x1FF;
        entry.affine = (entry.attr0 & (1 << 8)) != 0;
        entry.disabled = !entry.affine && ((entry.attr0 & (1 << 9)) != 0);
        entry.mode = (entry.attr0 >> 10) & 0x3;
        entry.mosaic = (entry.attr0 & (1 << 12)) != 0;
        entry.bpp8 = (entry.attr0 & (1 << 13)) != 0;
        entry.shape = (entry.attr0 >> 14) & 0x3;
        entry.size = (entry.attr1 >> 14) & 0x3;
        entry.tile = entry.attr2 & 0x3FF;
        entry.priority = (entry.attr2 >> 10) & 0x3;
        entry.palette = (entry.attr2 >> 12) & 0xF;
        entry.bitmap = entry.mode == 3;
        entry.rsGroup = entry.affine ? ((entry.attr1 >> 9) & 0x1F) : -1;

        decodeSize(entry.shape, entry.size, entry.width, entry.height);
        entry.used = !entry.disabled && entry.shape != 3
            && entry.width > 0 && entry.height > 0;

        if (entry.affine && entry.rsGroup >= 0)
        {
            const int rs = 0x06 + entry.rsGroup * 0x20;
            entry.pa = read16(oam, rs + 0x00);
            entry.pb = read16(oam, rs + 0x08);
            entry.pc = read16(oam, rs + 0x10);
            entry.pd = read16(oam, rs + 0x18);
        }

        entry.tileAddr = tileAddress(entry, dispcnt, objmask, oneD);
        entry.image = renderSprite(entry, objvram, objmask, palette, oneD);
        if (entry.used)
            used++;

        entries[index] = entry;
    }

    grid->update();
    updateDetails();
    statusLabel->setText(QString("Engine %1 - %2 used / 128")
                             .arg(engineB ? "B" : "A")
                             .arg(used));
}

void OAMViewerDialog::updateDetails()
{
    const OAMEntry& entry = entries[selectedIndex];
    const bool engineB = engineBox->currentIndex() == 1;
    const u32 oamBase = engineB ? 0x07000400u : 0x07000000u;
    const u32 objBase = engineB ? kObjBaseB : kObjBaseA;
    const u32 oamAddress = oamBase + u32(selectedIndex) * 8;
    const u32 tileAddress = objBase + entry.tileAddr;
    const u32 rsAddress = oamBase + 0x06 + u32(qMax(0, entry.rsGroup)) * 0x20;
    const bool doubleSize = entry.affine && (entry.attr0 & (1 << 9));
    const bool hflip = !entry.affine && (entry.attr1 & (1 << 12));
    const bool vflip = !entry.affine && (entry.attr1 & (1 << 13));

    QString text;
    text += QString("OAM No.       %1\n").arg(selectedIndex, 3, 10, QChar('0'));
    text += QString("X-Loc         %1\n").arg(entry.x, 3, 10, QChar('0'));
    text += QString("Y-Loc         %1\n").arg(entry.y, 3, 10, QChar('0'));
    text += QString("Size          %1x%2\n").arg(entry.width).arg(entry.height);
    text += QString("OAM Data      %1,%2,%3\n")
                .arg(hex(entry.attr0, 4))
                .arg(hex(entry.attr1, 4))
                .arg(hex(entry.attr2, 4));
    text += QString("OAM Address   0x%1\n").arg(hex(oamAddress, 8));
    text += QString("Tile Address  0x%1\n").arg(hex(tileAddress, 8));
    text += QString("OBJ Mode      %1\n").arg(modeName(entry.mode));
    text += QString("OBJ Type      %1\n").arg(typeName(entry));
    text += QString("Palette       %1\n").arg(entry.palette);
    text += QString("Priority      %1\n").arg(entry.priority);
    text += QString("X-Flip        %1\n").arg(entry.affine ? "-" : (hflip ? "1" : "0"));
    text += QString("Y-Flip        %1\n").arg(entry.affine ? "-" : (vflip ? "1" : "0"));
    text += QString("Mosaic        %1\n").arg(entry.mosaic ? "1" : "0");
    text += QString("Rot/Scal      %1\n")
                .arg(entry.affine ? (doubleSize ? "On (double)" : "On") : "Off");
    if (entry.affine && entry.rsGroup >= 0)
    {
        text += QString("A-D           %1,%2,%3,%4\n")
                    .arg(hex(entry.pa, 4))
                    .arg(hex(entry.pb, 4))
                    .arg(hex(entry.pc, 4))
                    .arg(hex(entry.pd, 4));
        text += QString("R/S Address   0x%1\n").arg(hex(rsAddress, 8));
    }
    else
    {
        text += "A-D           -\n";
        text += "R/S Address   -\n";
    }

    details->setPlainText(text);
    preview->setEntry(entry, selectedIndex);
}
