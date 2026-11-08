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

namespace melonDS::NDSCart { class CartCommon; }

// Bounded ring buffer for the traces: fixed capacity, O(1) push (no allocation on
// the emulation thread), drops the oldest item on overflow and counts the drops.
template <typename T>
struct TraceRing
{
    std::vector<T> buf;
    size_t head = 0;
    size_t count = 0;
    uint64_t dropped = 0;

    void reset(size_t cap)
    {
        buf.assign(cap < 1 ? 1 : cap, T {});
        head = 0;
        count = 0;
        dropped = 0;
    }

    size_t size() const { return count; }

    void push(const T& item)
    {
        buf[head] = item;
        head = (head + 1) % buf.size();
        if (count < buf.size())
            count++;
        else
            dropped++; // overwrote the oldest
    }

    // Copies the held items out, oldest first, and empties the ring.
    void drain(std::vector<T>& out)
    {
        out.clear();
        out.reserve(count);
        const size_t start = (head + buf.size() - count) % buf.size();
        for (size_t i = 0; i < count; i++)
            out.push_back(buf[(start + i) % buf.size()]);
        head = 0;
        count = 0;
    }
};

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

        // DMA transfer trace subscription
        bool dmaTrace = false;
        int dmaBatch = 256;
        int dmaTimeoutMs = 100;
        int dmaEventFd = -1;
    };

    struct CartRead
    {
        uint32_t addr;
        uint32_t len;
        uint8_t cmd;
    };

    struct DmaEvent
    {
        uint32_t cpu;
        uint32_t ch;
        uint32_t src;
        uint32_t dst;
        uint32_t cnt;
        uint32_t pc;
    };

    struct GXEvent
    {
        uint32_t cmd;
        uint32_t param;
        uint32_t pc;
        uint32_t frame;
        uint32_t fifo;
    };

    struct OriginEvent
    {
        uint32_t dstLo;
        uint32_t dstHi;
        uint32_t src;
        uint32_t len;
        uint32_t pc;
        uint32_t frame;
        uint32_t kind; // 0 = DMA
    };

    struct Gpu3DVertex
    {
        int32_t x, y, z, w;
        uint8_t r, g, b;
        int16_t u, v;
    };

    struct Gpu3DPoly
    {
        uint32_t attr;
        uint32_t texParam;
        uint32_t texPalette;
        int32_t type;
        uint32_t numVertices;
        bool translucent;
        Gpu3DVertex verts[10];
    };

    struct Gpu3DSnapshot
    {
        bool valid = false;
        bool geometryEnabled = false;
        bool renderingEnabled = false;
        uint32_t frame = 0;
        uint32_t dispCnt = 0;
        uint32_t gxStat = 0;
        uint32_t fifoCmd = 0, fifoPipe = 0, fifoStall = 0;
        int32_t matrixMode = 0;
        int32_t viewport[6] = {};
        int32_t proj[16] = {}, pos[16] = {}, vec[16] = {}, tex[16] = {};
        uint32_t fogColor = 0, fogOffset = 0;
        uint32_t numVertices = 0, numPolygons = 0, renderPolygons = 0;
        std::vector<Gpu3DPoly> polys;
        // unique (texParam << 32 | texPalette)
        std::vector<uint64_t> textures;
    };

    void acceptLoop();
    void clientLoop(Client* client);

    // Returns true if the client should be kept alive.
    bool handleMessage(Client* client, const std::string& body);
    // Dispatch a parsed request (wrapped by handleMessage for logging).
    bool handleRequest(Client* client, const std::string& body);
    void sendMessage(Client* client, const std::string& body);
    void sendBinary(Client* client, const std::vector<uint8_t>& data);

    // Sends one streamed frame (event.frame notification + binary frame).
    void sendStreamSample(Client* client);

    // Cart read trace: called on the emu thread at each read transfer.
    void onCartRead(uint32_t addr, uint32_t len, uint8_t cmd);
    // Drains the trace buffer and sends a batch (event.cart) to the client.
    void flushCart(Client* client);
    void stopCartTrace(Client* client);

    // DMA transfer trace: called on the emu thread at each transfer start.
    void onDmaTransfer(uint32_t cpu, uint32_t ch, uint32_t src, uint32_t dst,
                       uint32_t cnt, uint32_t pc);
    // Drains the trace buffer and sends a batch (event.dma) to the client.
    void flushDma(Client* client);
    void stopDmaTrace(Client* client);

    // 3D snapshot: captured at the next VBlank (one-shot, on demand).
    void onVBlank();
    bool requestSnapshot(Gpu3DSnapshot& out, int timeoutMs = 2000);

    // GXFIFO command trace: called on the emu thread for every enqueued entry.
    void onGXCommand(uint32_t cmd, uint32_t param, uint32_t pc, uint32_t frame, uint32_t fifo);
    void stopGXTrace();

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
    TraceRing<CartRead> cartRing;
    bool cartActive;
    int cartBatchN;
    int cartEventFd;

    // DMA transfer trace (single subscriber at a time)
    std::mutex dmaMutex;
    TraceRing<DmaEvent> dmaRing;
    bool dmaActive;
    int dmaBatchN;
    int dmaEventFd;

    // 3D snapshot (one-shot at VBlank)
    std::mutex snapshotMutex;
    std::condition_variable snapshotCv;
    bool snapshotPending = false;
    bool snapshotReady = false;
    Gpu3DSnapshot snapshotData;

    // GXFIFO command trace
    std::mutex gxMutex;
    TraceRing<GXEvent> gxRing;
    bool gxActive = false;

    // VRAM write-origin history (DMA; armed on demand)
    std::mutex originMutex;
    TraceRing<OriginEvent> originRing;
    bool originActive = false;

    // ROM info: the checksum is computed once per loaded cart and cached by identity.
    std::mutex romMutex;
    const melonDS::NDSCart::CartCommon* checksumCart = nullptr;
    std::string checksumName;
    uint32_t checksumLength = 0;
    uint32_t checksumValue = 0;

    // opt-in traffic logging (MELONDS_CONTROL_LOG); off by default
    bool logTraffic;
};

#endif
