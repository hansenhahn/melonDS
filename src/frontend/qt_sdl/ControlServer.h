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

#ifndef CONTROLSERVER_H
#define CONTROLSERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EmuInstance;

// Control channel for external agents (MCP). Runs on its own thread and never
// touches the emulation core directly: input is applied by the emu thread from
// shared fields, screen capture is copied by the emu thread on demand, and core
// operations are queued to the emu thread.
class ControlServer
{
public:
    explicit ControlServer(EmuInstance* inst);
    ~ControlServer();

    bool start(const std::string& path);
    void stop();

    // Runs on the emu thread (from a queued message). Copies the framebuffer
    // into the staging buffer only when a capture was requested.
    void serviceCapture();

private:
    struct Client
    {
        int fd;
        std::mutex writeMutex;

        // screen capture streaming (screen.capture with every_ms)
        bool streaming = false;
        int streamEveryMs = 0;
        int streamRemaining = -1;      // -1 = until duration
        int streamSent = 0;
        long long streamNextAtMs = 0;
        long long streamEndAtMs = -1;  // -1 = until count
        std::string streamScreens;
        std::string streamFormat;

        // cart read trace subscription
        bool cartTrace = false;
        int cartBatch = 256;
        int cartTimeoutMs = 100;
        int cartEventFd = -1;
    };

    struct CartRead
    {
        uint32_t addr;
        uint32_t len;
        uint8_t cmd;
    };

    void acceptLoop();
    void clientLoop(Client* client);

    // Returns true if the client should be kept alive.
    bool handleMessage(Client* client, const std::string& body);
    void sendMessage(Client* client, const std::string& body);
    void sendBinary(Client* client, const std::vector<uint8_t>& data);

    // Sends one streamed frame (event.frame notification + binary frame).
    void sendStreamSample(Client* client);

    // Cart read trace: called on the emu thread at each read transfer.
    void onCartRead(uint32_t addr, uint32_t len, uint8_t cmd);
    // Drains the trace buffer and sends a batch (event.cart) to the client.
    void flushCart(Client* client);
    void stopCartTrace(Client* client);

    // Requests a capture from the emu thread and waits for it. Returns false
    // (with err set) if the framebuffer is unavailable (e.g. OpenGL renderer).
    bool capture(int screens, const std::string& format,
                 std::vector<uint8_t>& out, int& width, int& height, std::string& err);

    EmuInstance* instance;

    int listenFd;
    std::string socketPath;
    std::thread acceptThread;
    std::atomic<bool> running;

    std::mutex clientsMutex;
    std::vector<Client*> clients;

    // screen capture handshake with the emu thread
    std::mutex captureMutex;
    std::condition_variable captureCv;
    bool captureRequested;
    bool captureDone;
    bool captureValid;
    int captureScreens;
    // when >= 0, serviceCapture re-renders the current frame filtered to one
    // layer (engine 0/1, bit: bg0-3=0-3, obj=4, 3d=0) instead of just copying.
    int captureLayerEngine = -1;
    int captureLayerBit = 0;
    std::vector<uint8_t> captureBuf;

    // cart read trace (single subscriber at a time)
    std::mutex cartMutex;
    std::vector<CartRead> cartBuf;
    bool cartOverflow;
    bool cartActive;
    int cartBatchN;
    int cartEventFd;
};

#endif
