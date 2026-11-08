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

#ifndef EMUTHREAD_H
#define EMUTHREAD_H

#include <QThread>
#include <QMutex>
#include <QSemaphore>
#include <QWaitCondition>
#include <QQueue>
#include <QVariant>

#include <atomic>
#include <variant>
#include <optional>
#include <list>

#include "NDSCart.h"
#include "GBACart.h"

namespace melonDS
{
class NDS;
}

class EmuInstance;
class MainWindow;
class ScreenPanelGL;

class EmuThread : public QThread
{
    Q_OBJECT
    void run() override;

public:
    explicit EmuThread(EmuInstance* inst, QObject* parent = nullptr);

    void attachWindow(MainWindow* window);
    void detachWindow(MainWindow* window);

    enum MessageType
    {
        msg_Exit,

        msg_EmuRun,
        msg_EmuPause,
        msg_EmuUnpause,
        msg_EmuStop,
        msg_EmuFrameStep,
        msg_EmuReset,

        msg_InitGL,
        msg_DeInitGL,
        msg_BorrowGL,

        msg_BootROM,
        msg_BootFirmware,
        msg_InsertCart,
        msg_EjectCart,
        msg_InsertGBACart,
        msg_InsertGBAAddon,
        msg_EjectGBACart,

        msg_LoadState,
        msg_SaveState,
        msg_UndoStateLoad,

        msg_ImportSavefile,

        msg_EnableCheats,

        msg_ControlCapture,
        msg_EmuFrameCounter,
    };

    struct Message
    {
        MessageType type;
        QVariant param;
    };

    void sendMessage(Message msg);
    void waitMessage(int num = 1);
    void waitAllMessages();

    void sendMessage(MessageType type)
    {
        return sendMessage({.type = type});
    }

    void changeWindowTitle(char* title);

    // to be called from the UI thread
    void emuRun();
    void emuPause(bool broadcast = true);
    void emuUnpause(bool broadcast = true);
    void emuTogglePause(bool broadcast = true);
    void emuStop(bool external);
    void emuExit();
    void emuFrameStep();
    // Advances exactly n frames and stops. With snapshot=true, records the emu
    // frame at which the 3D snapshot must be taken (the last frame of the step)
    // so the caller can capture it atomically before the halt.
    void emuStepFrames(int n, bool snapshot = false);
    // Reads the frame counter on the emu thread (safe/consistent from any thread).
    uint32_t emuFrameCounter();
    // Frame targeted by the in-progress step snapshot (0 when not armed).
    uint32_t frameStepSnapshotTarget() const
    {
        return frameStepSnapshotTargetFrame.load(std::memory_order_relaxed);
    }
    void emuReset(bool reconfigure = true);

    int bootROM(const QStringList& filename, QString& errorstr);
    int bootFirmware(QString& errorstr);
    int insertCart(const QStringList& filename, bool gba, QString& errorstr);
    void ejectCart(bool gba);
    int insertGBAAddon(int type, QString& errorstr);

    int saveState(const QString& filename);
    int loadState(const QString& filename);
    // Frame/PC observed on the emu thread right after the last savestate
    // save/load (for metadata and round-trip verification).
    uint32_t lastStateFrame() const { return msgSavedFrame; }
    uint32_t lastStatePC() const { return msgSavedPC; }
    int undoStateLoad();

    int importSavefile(const QString& filename);

    void enableCheats(bool enable);

    bool emuIsRunning();
    bool emuIsActive();

    void initContext(int win);
    void deinitContext(int win);
    void borrowGL();
    void returnGL();
    void updateVideoSettings() { videoSettingsDirty = true; }
    void updateVideoRenderer() { videoSettingsDirty = true; lastVideoRenderer = -1; }

    QWaitCondition glBorrowCond;
    QMutex glBorrowMutex;

signals:
    void windowUpdate();
    void windowTitleChange(QString title);

    void windowEmuStart();
    void windowEmuStop();
    void windowEmuPause(bool pause);
    void windowEmuReset();

    void windowLimitFPSChange();

    void autoScreenSizingChange(int sizing);

    void windowFullscreenToggle();

    void swapScreensToggle();
    void screenEmphasisToggle();

    void syncVolumeLevel();

private:
    void handleMessages();

    void updateRenderer();
    void compileShaders();

    enum EmuStatusKind
    {
        emuStatus_Exit,
        emuStatus_Running,
        emuStatus_Paused,
        emuStatus_FrameStep,
    };

    EmuStatusKind prevEmuStatus;
    EmuStatusKind emuStatus;
    bool emuActive;

    constexpr static int emuPauseStackRunning = 0;
    constexpr static int emuPauseStackPauseThreshold = 1;
    int emuPauseStack;

    int msgResult = 0;
    QString msgError;
    uint32_t msgFrameCounter = 0;
    uint32_t msgSavedFrame = 0;
    uint32_t msgSavedPC = 0;

    QMutex msgMutex;
    QSemaphore msgSemaphore;
    QQueue<Message> msgQueue;

    // frame stepping (emu.step_frame): the emu thread runs one frame per loop
    // iteration until frameStepCount reaches 0, then signals frameStepDone.
    int frameStepCount = 0;
    bool frameStepPending = false;
    QMutex frameStepMutex;
    QWaitCondition frameStepDone;

    // Set by the caller before queueing msg_EmuFrameStep (true = capture the 3D
    // snapshot on the last frame). Consumed by the emu thread, which stores the
    // absolute target frame; 0 means "not armed".
    std::atomic<bool> frameStepSnapshotRequest{false};
    std::atomic<uint32_t> frameStepSnapshotTargetFrame{0};

    EmuInstance* emuInstance;

    int autoScreenSizing;

    int lastVideoRenderer = -1;

    double perfCountsSec;

    bool useOpenGL;
    int videoRenderer;
    bool videoSettingsDirty;
};

#endif // EMUTHREAD_H
