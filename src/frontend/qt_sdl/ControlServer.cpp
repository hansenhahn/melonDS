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

#include "ControlServer.h"

#include "EmuInstance.h"
#include "NDS.h"

#include "thirdparty/nlohmann/json.hpp"

using nlohmann::json;

#include <algorithm>
#include <chrono>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#endif

// Fixed default capacity of the trace rings (items dropped, oldest first, on overflow).
static constexpr int TRACE_RING_CAP = 65536;

ControlServer::ControlServer(EmuInstance* inst) : instance(inst), listenFd(-1), running(false),
    captureRequested(false), captureDone(false), captureValid(false), captureScreens(2),
    cartActive(false), cartBatchN(256), cartEventFd(-1),
    dmaActive(false), dmaBatchN(256), dmaEventFd(-1)
{
}

ControlServer::~ControlServer()
{
    stop();
}

#ifndef _WIN32

// Reads a single CRLF-terminated header line. Returns false on EOF/error.
static bool ReadLine(int fd, std::string& line)
{
    line.clear();
    char c;
    while (true)
    {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n')
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(c);
    }
}

static bool ReadExact(int fd, char* buf, size_t len)
{
    size_t got = 0;
    while (got < len)
    {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ControlServer::start(const std::string& path)
{
    if (running) return true;

    listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0) return false;

    // remove a stale socket file
    unlink(path.c_str());

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        close(listenFd);
        listenFd = -1;
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(listenFd, 4) < 0)
    {
        close(listenFd);
        listenFd = -1;
        return false;
    }

    socketPath = path;
    running = true;
    acceptThread = std::thread(&ControlServer::acceptLoop, this);
    return true;
}

void ControlServer::stop()
{
    if (!running) return;
    running = false;

    if (listenFd >= 0)
    {
        shutdown(listenFd, SHUT_RDWR);
        close(listenFd);
        listenFd = -1;
    }
    if (acceptThread.joinable()) acceptThread.join();

    // Wake up the client threads; they clean themselves up.
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (Client* c : clients)
        shutdown(c->fd, SHUT_RDWR);
    clients.clear();

    if (!socketPath.empty()) unlink(socketPath.c_str());
}

void ControlServer::acceptLoop()
{
    while (running)
    {
        int fd = accept(listenFd, nullptr, nullptr);
        if (fd < 0)
        {
            if (!running) break;
            continue;
        }

        Client* c = new Client {fd};
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.push_back(c);
        }
        std::thread(&ControlServer::clientLoop, this, c).detach();
    }
}

void ControlServer::clientLoop(Client* client)
{
    int fd = client->fd;

    while (running)
    {
        int timeout = -1;
        if (client->streaming)
        {
            long long wait = client->streamNextAtMs - nowMs();
            timeout = (wait < 0) ? 0 : (int)wait;
        }
        if (client->cartTrace)
        {
            int t = client->cartTimeoutMs;
            timeout = (timeout < 0 || t < timeout) ? t : timeout;
        }
        if (client->dmaTrace)
        {
            int t = client->dmaTimeoutMs;
            timeout = (timeout < 0 || t < timeout) ? t : timeout;
        }

        struct pollfd pfds[3];
        int nfds = 1;
        int cartIdx = -1, dmaIdx = -1;
        pfds[0] = {fd, POLLIN, 0};
        if (client->cartTrace && client->cartEventFd >= 0)
        {
            cartIdx = nfds;
            pfds[nfds++] = {client->cartEventFd, POLLIN, 0};
        }
        if (client->dmaTrace && client->dmaEventFd >= 0)
        {
            dmaIdx = nfds;
            pfds[nfds++] = {client->dmaEventFd, POLLIN, 0};
        }

        int pr = poll(pfds, nfds, timeout);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0)
        {
            // streaming: a sample is due; traces: flush the pending batches
            if (client->streaming) sendStreamSample(client);
            if (client->cartTrace) flushCart(client);
            if (client->dmaTrace) flushDma(client);
            continue;
        }

        // cart batch signal
        if (cartIdx >= 0 && (pfds[cartIdx].revents & POLLIN))
        {
            uint64_t v;
            ssize_t r = read(client->cartEventFd, &v, sizeof(v));
            (void)r;
            flushCart(client);
        }

        // DMA batch signal
        if (dmaIdx >= 0 && (pfds[dmaIdx].revents & POLLIN))
        {
            uint64_t v;
            ssize_t r = read(client->dmaEventFd, &v, sizeof(v));
            (void)r;
            flushDma(client);
        }

        if (!(pfds[0].revents & POLLIN)) continue;

        // read one message (headers, then body)
        size_t contentLength = 0;
        bool haveLength = false;
        bool eof = false;
        std::string line;
        while (true)
        {
            if (!ReadLine(fd, line)) { eof = true; break; }
            if (line.empty()) break;
            const std::string key = "Content-Length:";
            if (line.compare(0, key.size(), key) == 0)
            {
                contentLength = strtoul(line.c_str() + key.size(), nullptr, 10);
                haveLength = true;
            }
        }
        if (eof || !haveLength) break;

        std::string body;
        body.resize(contentLength);
        if (!ReadExact(fd, &body[0], contentLength)) break;
        if (!handleMessage(client, body)) break;
    }

    stopCartTrace(client);
    stopDmaTrace(client);

    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto it = clients.begin(); it != clients.end(); ++it)
        {
            if (*it == client)
            {
                clients.erase(it);
                break;
            }
        }
    }
    close(fd);
    delete client;
}

// Sends one streamed frame: an event.frame notification announcing the binary
// payload, immediately followed by the raw binary frame.
void ControlServer::sendStreamSample(Client* client)
{
    int screens = (client->streamScreens == "top") ? 0
                : (client->streamScreens == "bottom") ? 1 : 2;

    std::vector<uint8_t> buf;
    int w = 0, h = 0;
    std::string err;
    if (capture(screens, client->streamFormat, buf, w, h, err))
    {
        json note = {{"jsonrpc", "2.0"}, {"method", "event.frame"},
                     {"params", {{"binary", {{"bytes", buf.size()}, {"width", w}, {"height", h},
                                             {"screens", client->streamScreens}, {"format", client->streamFormat}}}}}};
        sendMessage(client, note.dump());
        sendBinary(client, buf);
        client->streamSent++;
    }

    bool stop = false;
    if (client->streamRemaining > 0)
    {
        client->streamRemaining--;
        if (client->streamRemaining == 0) stop = true;
    }

    long long next = nowMs() + client->streamEveryMs;
    if (client->streamEndAtMs >= 0 && next >= client->streamEndAtMs) stop = true;

    if (stop)
    {
        json end = {{"jsonrpc", "2.0"}, {"method", "event.stream_end"},
                    {"params", {{"count", client->streamSent}}}};
        sendMessage(client, end.dump());
        client->streaming = false;
        return;
    }
    client->streamNextAtMs = next;
}

// Runs on the emu thread. Appends the read (O(1)) and signals the subscriber's
// eventfd when a full batch is reached. No JSON, no socket I/O here.
void ControlServer::onCartRead(uint32_t addr, uint32_t len, uint8_t cmd)
{
    std::lock_guard<std::mutex> lk(cartMutex);
    if (!cartActive) return;

    cartRing.push({addr, len, cmd});
    if ((int)cartRing.size() >= cartBatchN && cartEventFd >= 0)
    {
        uint64_t one = 1;
        ssize_t r = write(cartEventFd, &one, sizeof(one));
        (void)r;
    }
}

// Runs on the subscriber's client thread. Drains the buffer and sends a batch.
void ControlServer::flushCart(Client* client)
{
    std::vector<CartRead> batch;
    uint64_t dropped;
    {
        std::lock_guard<std::mutex> lk(cartMutex);
        cartRing.drain(batch);
        dropped = cartRing.dropped;
        cartRing.dropped = 0;
    }
    if (batch.empty() && !dropped) return;

    json reads = json::array();
    for (const CartRead& r : batch)
        reads.push_back({{"addr", r.addr}, {"len", r.len}, {"cmd", r.cmd}});

    json note = {{"jsonrpc", "2.0"}, {"method", "event.cart"},
                 {"params", {{"reads", reads}, {"count", batch.size()}, {"dropped", dropped}}}};
    sendMessage(client, note.dump());
}

void ControlServer::stopCartTrace(Client* client)
{
    if (!client->cartTrace) return;

    melonDS::NDS* nds = instance->getNDS();
    if (nds) nds->NDSCartSlot.OnROMRead = nullptr;

    {
        std::lock_guard<std::mutex> lk(cartMutex);
        cartActive = false;
        cartEventFd = -1;
        cartRing.reset(0);
    }
    if (client->cartEventFd >= 0)
    {
        close(client->cartEventFd);
        client->cartEventFd = -1;
    }
    client->cartTrace = false;
}

// Runs on the emu thread. Appends the transfer (O(1)) and signals the subscriber's
// eventfd when a full batch is reached. No JSON, no socket I/O here.
void ControlServer::onDmaTransfer(uint32_t cpu, uint32_t ch, uint32_t src, uint32_t dst,
                                  uint32_t cnt, uint32_t pc)
{
    std::lock_guard<std::mutex> lk(dmaMutex);
    if (!dmaActive) return;

    dmaRing.push({cpu, ch, src, dst, cnt, pc});
    if ((int)dmaRing.size() >= dmaBatchN && dmaEventFd >= 0)
    {
        uint64_t one = 1;
        ssize_t r = write(dmaEventFd, &one, sizeof(one));
        (void)r;
    }
}

// Runs on the subscriber's client thread. Drains the buffer and sends a batch.
void ControlServer::flushDma(Client* client)
{
    std::vector<DmaEvent> batch;
    uint64_t dropped;
    {
        std::lock_guard<std::mutex> lk(dmaMutex);
        dmaRing.drain(batch);
        dropped = dmaRing.dropped;
        dmaRing.dropped = 0;
    }
    if (batch.empty() && !dropped) return;

    json transfers = json::array();
    for (const DmaEvent& t : batch)
        transfers.push_back({{"cpu", t.cpu}, {"ch", t.ch}, {"src", t.src},
                             {"dst", t.dst}, {"cnt", t.cnt}, {"pc", t.pc}});

    json note = {{"jsonrpc", "2.0"}, {"method", "event.dma"},
                 {"params", {{"transfers", transfers}, {"count", batch.size()},
                             {"dropped", dropped}}}};
    sendMessage(client, note.dump());
}

void ControlServer::stopDmaTrace(Client* client)
{
    if (!client->dmaTrace) return;

    melonDS::NDS* nds = instance->getNDS();
    if (nds) nds->OnDmaTransfer = nullptr;

    {
        std::lock_guard<std::mutex> lk(dmaMutex);
        dmaActive = false;
        dmaEventFd = -1;
        dmaRing.reset(0);
    }
    if (client->dmaEventFd >= 0)
    {
        close(client->dmaEventFd);
        client->dmaEventFd = -1;
    }
    client->dmaTrace = false;
}

void ControlServer::sendMessage(Client* client, const std::string& body)
{
    std::string out = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::lock_guard<std::mutex> lk(client->writeMutex);
    size_t sent = 0;
    while (sent < out.size())
    {
        ssize_t n = send(client->fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

// Binary frame: [u32 BE N][N bytes]. Sent right after a JSON-RPC response that
// announces the binary payload (see screen.capture).
void ControlServer::sendBinary(Client* client, const std::vector<uint8_t>& data)
{
    uint32_t n = (uint32_t)data.size();
    uint8_t hdr[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};

    std::lock_guard<std::mutex> lk(client->writeMutex);
    size_t sent = 0;
    while (sent < 4)
    {
        ssize_t r = send(client->fd, hdr + sent, 4 - sent, MSG_NOSIGNAL);
        if (r <= 0) return;
        sent += (size_t)r;
    }
    sent = 0;
    while (sent < data.size())
    {
        ssize_t r = send(client->fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (r <= 0) return;
        sent += (size_t)r;
    }
}

// Runs on the emu thread. Copies the framebuffer into the staging buffer only
// when a capture was requested, then wakes the requesting control thread.
void ControlServer::serviceCapture()
{
    std::lock_guard<std::mutex> lk(captureMutex);
    if (!captureRequested) return;

    captureRequested = false;
    captureValid = false;

    melonDS::NDS* nds = instance->getNDS();
    void* top = nullptr;
    void* bottom = nullptr;

    // Optional isolated re-render of the current frame, filtered to one layer.
    // The composed output goes to scratch buffers (the live framebuffer is never
    // touched) and all GPU state is restored on scope exit.
    if (nds && captureLayerEngine >= 0)
    {
        static uint32_t layerScratch[2][256 * 192];

        melonDS::GPU& gpu = nds->GPU;
        melonDS::Renderer& rend = gpu.GetRenderer();

        // RAII: always restore renderer + GPU2D state, even on early return
        struct LayerGuard
        {
            melonDS::GPU& gpu;
            uint16_t savedVCount;
            uint8_t savedA, savedB;
            ~LayerGuard()
            {
                gpu.GetRenderer().SetExternalRenderTarget(nullptr, nullptr);
                gpu.GetRenderer().Set3DExternalScanlineRead(false);
                gpu.GPU2D_A.RenderMask = savedA;
                gpu.GPU2D_B.RenderMask = savedB;
                gpu.VCount = savedVCount;
            }
        } guard { gpu, gpu.VCount, gpu.GPU2D_A.RenderMask, gpu.GPU2D_B.RenderMask };

        gpu.GPU2D_A.RenderMask = (captureLayerEngine == 0) ? (uint8_t)captureLayerBit : 0;
        gpu.GPU2D_B.RenderMask = (captureLayerEngine == 1) ? (uint8_t)captureLayerBit : 0;

        rend.SetExternalRenderTarget(layerScratch[0], layerScratch[1]);
        // the 3D render thread is idle while paused, so don't block waiting for it:
        // the last rendered 3D frame is already in its buffer.
        rend.Set3DExternalScanlineRead(true);

        // reset pass (mirrors StartHBlank at VCount==262): latch regs w/ resetregs=true
        // and pre-render sprites for line 0
        gpu.VCount = 262;
        gpu.GPU2D_A.UpdateRegistersPreDraw(true);
        gpu.GPU2D_B.UpdateRegistersPreDraw(true);
        rend.DrawSprites(0);
        gpu.GPU2D_A.UpdateRegistersPostDraw(true);
        gpu.GPU2D_B.UpdateRegistersPostDraw(true);

        for (uint32_t line = 0; line < 192; line++)
        {
            gpu.VCount = line;                             // StartScanline
            gpu.GPU2D_A.UpdateWindows(line);
            gpu.GPU2D_B.UpdateWindows(line);
            gpu.GPU2D_A.UpdateRegistersPreDraw(false);     // StartHBlank
            gpu.GPU2D_B.UpdateRegistersPreDraw(false);
            rend.DrawScanline(line);
            if (line < 191)
                rend.DrawSprites(line + 1);
            gpu.GPU2D_A.UpdateRegistersPostDraw(false);
            gpu.GPU2D_B.UpdateRegistersPostDraw(false);
        }

        top = layerScratch[0];
        bottom = layerScratch[1];
    }
    else if (nds)
    {
        nds->GPU.GetFramebuffers(&top, &bottom);
    }

    if (top && bottom)
    {
        const size_t frameSize = 256 * 192 * 4;
        if (captureScreens == 2)
        {
            captureBuf.resize(frameSize * 2);
            memcpy(captureBuf.data(), top, frameSize);
            memcpy(captureBuf.data() + frameSize, bottom, frameSize);
        }
        else
        {
            captureBuf.resize(frameSize);
            memcpy(captureBuf.data(), (captureScreens == 0) ? top : bottom, frameSize);
        }
        captureValid = true;
    }

    captureDone = true;
    captureCv.notify_all();
}

bool ControlServer::capture(int screens, const std::string& format,
                            std::vector<uint8_t>& out, int& width, int& height, std::string& err)
{
    width = 256;
    height = 192;

    {
        std::lock_guard<std::mutex> lk(captureMutex);
        captureRequested = true;
        captureDone = false;
        captureScreens = screens;
    }

    instance->getEmuThread()->sendMessage({EmuThread::msg_ControlCapture});

    std::vector<uint8_t> raw;
    {
        std::unique_lock<std::mutex> lk(captureMutex);
        if (!captureCv.wait_for(lk, std::chrono::seconds(2), [&] { return captureDone; }))
        {
            captureRequested = false;
            err = "screen capture timed out";
            return false;
        }
        captureDone = false;
        if (!captureValid)
        {
            err = "screen capture requires the software renderer";
            return false;
        }
        raw.swap(captureBuf);
    }

    // Convert BGRA -> RGBA off the emu thread if requested.
    if (format == "rgba8")
    {
        for (size_t i = 0; i + 3 < raw.size(); i += 4)
            std::swap(raw[i], raw[i + 2]);
    }

    out.swap(raw);
    return true;
}

bool ControlServer::handleMessage(Client* client, const std::string& body)
{
    json req = json::parse(body, nullptr, false);
    if (req.is_discarded() || !req.is_object())
    {
        json err = {{"jsonrpc", "2.0"}, {"id", nullptr},
                    {"error", {{"code", -32700}, {"message", "parse error"}}}};
        sendMessage(client, err.dump());
        return true;
    }

    // notification: no id -> no response
    bool isNotification = !req.contains("id");
    json id = isNotification ? json(nullptr) : req["id"];
    std::string method = req.value("method", "");
    json params = req.value("params", json::object());

    melonDS::NDS* nds = instance->getNDS();

    auto ok = [&](const json& result) {
        if (!isNotification)
        {
            json resp = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            sendMessage(client, resp.dump());
        }
        return true;
    };
    auto fail = [&](int code, const std::string& msg) {
        if (!isNotification)
        {
            json resp = {{"jsonrpc", "2.0"}, {"id", id},
                         {"error", {{"code", code}, {"message", msg}}}};
            sendMessage(client, resp.dump());
        }
        return true;
    };

    if (method == "ping")
        return ok({{"pong", true}});

    if (method == "input.key")
    {
        if (!nds) return fail(-32000, "emulator not running");
        nds->AgentInputMask = params.value("mask", 0xFFFu);
        return ok({{"ok", true}});
    }
    if (method == "input.touch")
    {
        if (!nds) return fail(-32000, "emulator not running");
        nds->AgentTouching = true;
        nds->AgentTouchX = (melonDS::u16)params.value("x", 0);
        nds->AgentTouchY = (melonDS::u16)params.value("y", 0);
        return ok({{"ok", true}});
    }
    if (method == "input.release")
    {
        if (!nds) return fail(-32000, "emulator not running");
        nds->AgentInputMask = 0xFFF;
        nds->AgentTouching = false;
        return ok({{"ok", true}});
    }
    if (method == "input.merge")
    {
        if (!nds) return fail(-32000, "emulator not running");
        nds->AgentMerge = params.value("enabled", true);
        return ok({{"ok", true}});
    }

    if (method == "screen.capture")
    {
        if (!nds) return fail(-32000, "emulator not running");

        std::string scr = params.value("screens", std::string("both"));
        int screens = (scr == "top") ? 0 : (scr == "bottom") ? 1 : 2;
        std::string format = params.value("format", std::string("bgra8"));

        // streaming mode: every_ms + (count or duration_ms)
        int everyMs = params.value("every_ms", 0);
        int count = params.value("count", -1);
        int durationMs = params.value("duration_ms", -1);
        if (everyMs > 0 && (count > 0 || durationMs > 0))
        {
            client->streaming = true;
            client->streamEveryMs = everyMs;
            client->streamRemaining = (count > 0) ? count : -1;
            client->streamSent = 0;
            client->streamEndAtMs = (durationMs > 0) ? nowMs() + durationMs : -1;
            client->streamNextAtMs = nowMs();
            client->streamScreens = scr;
            client->streamFormat = format;
            return ok({{"streaming", true}, {"every_ms", everyMs},
                       {"count", count}, {"duration_ms", durationMs}});
        }

        std::vector<uint8_t> buf;
        int w = 0, h = 0;
        std::string err;
        if (!capture(screens, format, buf, w, h, err))
            return fail(-32000, err);

        if (!isNotification)
        {
            json result = {{"binary", {{"bytes", buf.size()}, {"width", w}, {"height", h},
                                       {"screens", scr}, {"format", format}}}};
            json resp = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            sendMessage(client, resp.dump());
            sendBinary(client, buf);
        }
        return true;
    }

    if (method == "video.render_layer")
    {
        if (!nds) return fail(-32000, "emulator not running");

        std::string eng = params.value("engine", std::string("A"));
        std::string layer = params.value("layer", std::string("obj"));
        std::string scr = params.value("screens", std::string("both"));
        std::string format = params.value("format", std::string("rgba8"));

        int engidx;
        if (eng == "A" || eng == "a") engidx = 0;
        else if (eng == "B" || eng == "b") engidx = 1;
        else return fail(-32602, "engine must be 'A' or 'B'");

        uint8_t bit;
        if (layer == "bg0") bit = 1<<0;
        else if (layer == "bg1") bit = 1<<1;
        else if (layer == "bg2") bit = 1<<2;
        else if (layer == "bg3") bit = 1<<3;
        else if (layer == "obj") bit = 1<<4;
        else if (layer == "3d") bit = 1<<0;
        else return fail(-32602, "layer must be bg0..bg3/obj/3d");
        if (layer == "3d" && engidx != 0)
            return fail(-32602, "3D only exists on engine A");

        int screens = (scr == "top") ? 0 : (scr == "bottom") ? 1 : 2;

        // re-render the frozen frame: pause around it, then ALWAYS unpause, so the
        // emulator's pause stack stays balanced (running stays running, paused
        // stays paused) no matter the previous state.
        instance->getEmuThread()->emuPause(false);
        captureLayerEngine = engidx;
        captureLayerBit = bit;

        std::vector<uint8_t> buf;
        int w = 0, h = 0;
        std::string err;
        bool okc = capture(screens, format, buf, w, h, err);

        captureLayerEngine = -1;
        captureLayerBit = 0;

        instance->getEmuThread()->emuUnpause(false);

        if (!okc) return fail(-32000, err);

        if (!isNotification)
        {
            json result = {{"binary", {{"bytes", buf.size()}, {"width", w}, {"height", h},
                                       {"screens", scr}, {"format", format}}},
                           {"engine", eng}, {"layer", layer}};
            json resp = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            sendMessage(client, resp.dump());
            sendBinary(client, buf);
        }
        return true;
    }

    if (method == "emu.pause")
    {
        instance->getEmuThread()->emuPause();
        return ok({{"ok", true}});
    }
    if (method == "emu.resume")
    {
        instance->getEmuThread()->emuUnpause();
        return ok({{"ok", true}});
    }
    if (method == "emu.step_frame")
    {
        int n = params.value("n", 1);
        if (n < 1) n = 1;
        instance->getEmuThread()->emuStepFrames(n);
        return ok({{"ok", true}, {"frames", n}});
    }
    if (method == "savestate.save" || method == "savestate.load")
    {
        if (!nds) return fail(-32000, "emulator not running");

        int slot = params.value("slot", 0);
        std::string path = params.value("path", std::string(""));
        if (path.empty())
            path = melonDS::Platform::GetLocalFilePath("savestate_" + std::to_string(slot) + ".sav");

        QString qpath = QString::fromStdString(path);
        int r = (method == "savestate.save")
              ? instance->getEmuThread()->saveState(qpath)
              : instance->getEmuThread()->loadState(qpath);
        if (!r) return fail(-32000, "savestate operation failed");
        return ok({{"ok", true}, {"path", path}});
    }

    if (method == "cart.trace.start")
    {
        if (!nds) return fail(-32000, "emulator not running");

        int batch = params.value("batch", 256);
        if (batch < 1) batch = 1;
        int timeoutMs = params.value("timeout_ms", 100);
        if (timeoutMs < 1) timeoutMs = 1;
        int maxItems = params.value("max_items", TRACE_RING_CAP);
        if (maxItems < 1) maxItems = 1;
        if (maxItems > (1 << 22)) maxItems = 1 << 22;

        {
            std::lock_guard<std::mutex> lk(cartMutex);
            if (cartActive) return fail(-32000, "cart trace already active");
            cartActive = true;
            cartBatchN = batch;
            cartRing.reset(maxItems);
        }

        client->cartTrace = true;
        client->cartBatch = batch;
        client->cartTimeoutMs = timeoutMs;
        client->cartEventFd = eventfd(0, EFD_NONBLOCK);
        {
            std::lock_guard<std::mutex> lk(cartMutex);
            cartEventFd = client->cartEventFd;
        }

        nds->NDSCartSlot.OnROMRead = [this](uint32_t a, uint32_t l, uint8_t c) { onCartRead(a, l, c); };
        return ok({{"ok", true}, {"batch", batch}, {"timeout_ms", timeoutMs}, {"max_items", maxItems}});
    }
    if (method == "cart.trace.stop")
    {
        if (!client->cartTrace) return fail(-32000, "no cart trace active");
        stopCartTrace(client);
        return ok({{"ok", true}});
    }

    if (method == "dma.trace.start")
    {
        if (!nds) return fail(-32000, "emulator not running");

        int batch = params.value("batch", 256);
        if (batch < 1) batch = 1;
        int timeoutMs = params.value("timeout_ms", 100);
        if (timeoutMs < 1) timeoutMs = 1;
        int maxItems = params.value("max_items", TRACE_RING_CAP);
        if (maxItems < 1) maxItems = 1;
        if (maxItems > (1 << 22)) maxItems = 1 << 22;

        {
            std::lock_guard<std::mutex> lk(dmaMutex);
            if (dmaActive) return fail(-32000, "DMA trace already active");
            dmaActive = true;
            dmaBatchN = batch;
            dmaRing.reset(maxItems);
        }

        client->dmaTrace = true;
        client->dmaBatch = batch;
        client->dmaTimeoutMs = timeoutMs;
        client->dmaEventFd = eventfd(0, EFD_NONBLOCK);
        {
            std::lock_guard<std::mutex> lk(dmaMutex);
            dmaEventFd = client->dmaEventFd;
        }

        nds->OnDmaTransfer = [this](uint32_t cpu, uint32_t ch, uint32_t src, uint32_t dst,
                                    uint32_t cnt, uint32_t pc) {
            onDmaTransfer(cpu, ch, src, dst, cnt, pc);
        };
        return ok({{"ok", true}, {"batch", batch}, {"timeout_ms", timeoutMs}, {"max_items", maxItems}});
    }
    if (method == "dma.trace.stop")
    {
        if (!client->dmaTrace) return fail(-32000, "no DMA trace active");
        stopDmaTrace(client);
        return ok({{"ok", true}});
    }

    return fail(-32601, "method not found");
}

#else // _WIN32

bool ControlServer::start(const std::string&) { return false; }
void ControlServer::stop() {}
void ControlServer::acceptLoop() {}
void ControlServer::clientLoop(Client*) {}
bool ControlServer::handleMessage(Client*, const std::string&) { return false; }
void ControlServer::sendMessage(Client*, const std::string&) {}
void ControlServer::sendBinary(Client*, const std::vector<uint8_t>&) {}
void ControlServer::sendStreamSample(Client*) {}
void ControlServer::serviceCapture() {}
bool ControlServer::capture(int, const std::string&, std::vector<uint8_t>&, int&, int&, std::string&) { return false; }
void ControlServer::onCartRead(uint32_t, uint32_t, uint8_t) {}
void ControlServer::flushCart(Client*) {}
void ControlServer::stopCartTrace(Client*) {}
void ControlServer::onDmaTransfer(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void ControlServer::flushDma(Client*) {}
void ControlServer::stopDmaTrace(Client*) {}

#endif // _WIN32
