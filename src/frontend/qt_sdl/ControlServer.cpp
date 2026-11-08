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
#include "Platform.h"

#include "GPU3DTextureExport.h"

#include "CRC32.h"

#include "thirdparty/nlohmann/json.hpp"

using nlohmann::json;

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#endif

// Fixed default capacity of the trace rings (items dropped, oldest first, on overflow).
static constexpr int TRACE_RING_CAP = 65536;

// Maximum bytes served by a single rom.read call (clients chunk larger reads).
static constexpr int ROM_READ_MAX = 16 * 1024 * 1024;

#ifdef GDBSTUB_ENABLED
static Gdb::GdbStub* GetHitStub(melonDS::NDS* nds, const std::string& core)
{
    if (!nds) return nullptr;
    if (core == "arm7") return &nds->ARM7->GdbStub;
    if (core == "arm9") return &nds->ARM9->GdbStub;
    return nullptr;
}
#endif

ControlServer::ControlServer(EmuInstance* inst) : instance(inst), listenFd(-1), running(false),
    captureRequested(false), captureDone(false), captureValid(false), captureScreens(2),
    cartActive(false), cartBatchN(256), cartEventFd(-1),
    dmaActive(false), dmaBatchN(256), dmaEventFd(-1), gxActive(false),
    originActive(false), logTraffic(false)
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

// Savestate metadata sidecar ("<state>.sav.re.json"): keeps the melonDS binary
// format untouched while adding integrity/round-trip data and ownership.
static std::string StateSidecarPath(const std::string& path)
{
    return path + ".re.json";
}

static bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff len = f.tellg();
    if (len < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)len);
    if (len > 0 && !f.read((char*)out.data(), (std::streamsize)len)) return false;
    return true;
}

static bool ReadJsonFile(const std::string& path, json& out)
{
    std::vector<uint8_t> data;
    if (!ReadFileBytes(path, data)) return false;
    try
    {
        out = json::parse(data.begin(), data.end());
    }
    catch (...)
    {
        return false;
    }
    return true;
}

static bool WriteJsonAtomic(const std::string& path, const json& value)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string body = value.dump(2);
        f.write(body.data(), (std::streamsize)body.size());
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

static bool StateFileDigest(const std::string& path, uint32_t& crc, uint64_t& size)
{
    std::vector<uint8_t> data;
    if (!ReadFileBytes(path, data)) return false;
    size = data.size();
    crc = melonDS::CRC32(data.data(), (int)data.size(), 0);
    return true;
}

// Restricts the named-state part to a safe filename component (no path tricks).
static bool SanitizeStateName(const std::string& name, std::string& out)
{
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == "..") return false;
    for (char c : name)
    {
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'))
            return false;
    }
    bool allDigits = true;
    for (char c : name)
        if (!std::isdigit((unsigned char)c)) { allDigits = false; break; }
    if (allDigits) return false; // keep the UI numeric-slot namespace separate
    out = name;
    return true;
}

// Non-blocking exclusive lock held during validation + load (or save).
class StateFileLock
{
public:
    explicit StateFileLock(const std::string& path)
    {
        fd = open((path + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) return;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            close(fd);
            fd = -1;
        }
    }
    ~StateFileLock()
    {
        if (fd >= 0)
        {
            flock(fd, LOCK_UN);
            close(fd);
        }
    }
    bool ok() const { return fd >= 0; }

private:
    int fd = -1;
};


// Human-readable GXFIFO command name (No$gba-style, lowercase).
static const char* GXCommandName(uint32_t cmd)
{
    switch (cmd & 0xFF)
    {
    case 0x00: return "nop";
    case 0x10: return "mtx_mode";
    case 0x11: return "mtx_push";
    case 0x12: return "mtx_pop";
    case 0x13: return "mtx_store";
    case 0x14: return "mtx_restore";
    case 0x15: return "mtx_identity";
    case 0x16: return "mtx_load_4x4";
    case 0x17: return "mtx_load_4x3";
    case 0x18: return "mtx_mult_4x4";
    case 0x19: return "mtx_mult_4x3";
    case 0x1A: return "mtx_mult_3x3";
    case 0x1B: return "mtx_scale";
    case 0x1C: return "mtx_trans";
    case 0x20: return "color";
    case 0x21: return "normal";
    case 0x22: return "texcoord";
    case 0x23: return "vtx_16";
    case 0x24: return "vtx_10";
    case 0x25: return "vtx_xy";
    case 0x26: return "vtx_xz";
    case 0x27: return "vtx_yz";
    case 0x28: return "vtx_diff";
    case 0x29: return "polygon_attr";
    case 0x2A: return "teximage_param";
    case 0x2B: return "pltt_base";
    case 0x30: return "dif_amb";
    case 0x31: return "spe_emi";
    case 0x32: return "light_vector";
    case 0x33: return "light_color";
    case 0x34: return "shininess";
    case 0x40: return "begin_vtxs";
    case 0x41: return "end_vtxs";
    case 0x50: return "swap_buffers";
    case 0x60: return "viewport";
    case 0x70: return "box_test";
    case 0x71: return "pos_test";
    case 0x72: return "vec_test";
    default: return "cmd";
    }
}

bool ControlServer::start(const std::string& path)
{
    if (running) return true;

    // opt-in traffic logging (method/duration/errors); off by default, no per-frame cost
    logTraffic = getenv("MELONDS_CONTROL_LOG") != nullptr;

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

    melonDS::NDS* nds = instance->getNDS();
    if (nds)
    {
        nds->OnVBlank = nullptr;
        nds->OnGXCommand = nullptr;
        nds->OnDmaTransfer = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(snapshotMutex);
        snapshotPending = false;
    }
    {
        std::lock_guard<std::mutex> lk(originMutex);
        originActive = false;
    }

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
    {
        std::lock_guard<std::mutex> lk(dmaMutex);
        if (dmaActive)
        {
            dmaRing.push({cpu, ch, src, dst, cnt, pc});
            if ((int)dmaRing.size() >= dmaBatchN && dmaEventFd >= 0)
            {
                uint64_t one = 1;
                ssize_t r = write(dmaEventFd, &one, sizeof(one));
                (void)r;
            }
        }
    }

    std::lock_guard<std::mutex> lk(originMutex);
    if (!originActive) return;

    const uint32_t cntmask = (cpu == 0) ? 0x001FFFFF : ((ch == 3) ? 0x0000FFFF : 0x00003FFF);
    uint32_t count = cnt & cntmask;
    if (!count) count = cntmask + 1;
    const uint32_t unit = (cnt & (1u << 26)) ? 4 : 2;
    const uint32_t bytes = count * unit;

    uint32_t lo = dst, hi = dst + bytes;
    switch ((cnt >> 21) & 0x3)
    {
    case 1: lo = dst - bytes; hi = dst; break;      // decrement
    case 2: hi = dst + unit; break;                 // fixed
    default: break;                                 // increment
    }

    melonDS::NDS* nds = instance->getNDS();
    originRing.push({lo, hi, src, bytes, pc, nds ? nds->NumFrames : 0, 0});
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
    if (nds)
    {
        bool keepOrigin;
        {
            std::lock_guard<std::mutex> lk(originMutex);
            keepOrigin = originActive;
        }
        if (!keepOrigin) nds->OnDmaTransfer = nullptr;
    }

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

// Runs on the emu thread at the end of VBlank, after RenderPolygonRAM was
// finalized. Copies the completed frame once (only when a snapshot was requested).
void ControlServer::onVBlank()
{
    std::unique_lock<std::mutex> lk(snapshotMutex);
    if (!snapshotPending) return;

    melonDS::NDS* nds = instance->getNDS();
    // NumFrames is incremented after the frame returns; +1 means "the frame
    // being completed", which is the numbering a stopped step reports.
    const uint32_t captureFrame = nds ? (nds->NumFrames + 1) : 0;
    if (snapshotStepTarget)
    {
        EmuThread* thread = instance->getEmuThread();
        const uint32_t target = thread ? thread->frameStepSnapshotTarget() : 0;
        if (target == 0 || captureFrame < target)
            return; // not the target frame yet: keep waiting for a later VBlank
    }

    snapshotPending = false;
    snapshotStepTarget = false;

    Gpu3DSnapshot s;
    if (nds)
    {
        melonDS::GPU3D& g = nds->GPU.GPU3D;
        s.valid = true;
        s.geometryEnabled = g.GeometryEnabled;
        s.renderingEnabled = g.RenderingEnabled;
        s.frame = captureFrame;
        s.dispCnt = g.DispCnt;
        s.gxStat = g.GXStat;
        s.fifoCmd = g.CmdFIFO.Level();
        s.fifoPipe = g.CmdPIPE.Level();
        s.fifoStall = g.CmdStallQueue.Level();
        s.matrixMode = g.MatrixMode;
        for (int i = 0; i < 6; i++) s.viewport[i] = g.Viewport[i];
        for (int i = 0; i < 16; i++)
        {
            s.proj[i] = g.ProjMatrix[i];
            s.pos[i] = g.PosMatrix[i];
            s.vec[i] = g.VecMatrix[i];
            s.tex[i] = g.TexMatrix[i];
        }
        s.fogColor = g.FogColor;
        s.fogOffset = g.FogOffset;
        s.numVertices = g.NumVertices;
        s.numPolygons = g.NumPolygons;

        uint32_t count = g.RenderNumPolygons;
        const uint32_t maxPolys = (uint32_t)g.RenderPolygonRAM.size();
        if (count > maxPolys) count = maxPolys;
        s.renderPolygons = count;
        s.polys.reserve(count);

        for (uint32_t i = 0; i < count; i++)
        {
            const melonDS::Polygon* p = g.RenderPolygonRAM[i];
            if (!p) continue;

            Gpu3DPoly pv {};
            pv.attr = p->Attr;
            pv.texParam = p->TexParam;
            pv.texPalette = p->TexPalette;
            pv.type = p->Type;
            pv.numVertices = p->NumVertices < 10 ? p->NumVertices : 10;
            pv.translucent = p->Translucent;
            for (uint32_t v = 0; v < pv.numVertices; v++)
            {
                const melonDS::Vertex* vv = p->Vertices[v];
                if (!vv) continue;
                Gpu3DVertex& out = pv.verts[v];
                out.x = vv->FinalPosition[0];
                out.y = vv->FinalPosition[1];
                out.z = p->FinalZ[v];
                out.w = p->FinalW[v];
                out.r = (uint8_t)(vv->FinalColor[0] >> 12);
                out.g = (uint8_t)(vv->FinalColor[1] >> 12);
                out.b = (uint8_t)(vv->FinalColor[2] >> 12);
                out.u = vv->TexCoords[0];
                out.v = vv->TexCoords[1];
            }
            s.polys.push_back(pv);

            if (((pv.texParam >> 26) & 0x7) != 0)
            {
                const uint64_t key = ((uint64_t)pv.texParam << 32) | pv.texPalette;
                bool found = false;
                for (uint64_t k : s.textures) { if (k == key) { found = true; break; } }
                if (!found) s.textures.push_back(key);
            }
        }
    }

    cachedSnapshot = std::move(s);
    hasCachedSnapshot = cachedSnapshot.valid;
    snapshotReady = true;
    lk.unlock();
    snapshotCv.notify_all();
}

bool ControlServer::requestSnapshot(Gpu3DSnapshot& out, int timeoutMs)
{
    melonDS::NDS* nds = instance->getNDS();
    if (!nds) return false;

    {
        std::lock_guard<std::mutex> lk(snapshotMutex);
        snapshotPending = true;
        snapshotStepTarget = false;
        snapshotReady = false;
    }

    nds->OnVBlank = [this] { onVBlank(); };

    std::unique_lock<std::mutex> lk(snapshotMutex);
    if (!snapshotCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                             [&] { return snapshotReady; }))
    {
        snapshotPending = false;
        return false;
    }

    out = cachedSnapshot;
    return out.valid;
}

void ControlServer::armStepSnapshot()
{
    melonDS::NDS* nds = instance->getNDS();
    if (!nds) return;
    {
        std::lock_guard<std::mutex> lk(snapshotMutex);
        snapshotPending = true;
        snapshotStepTarget = true;
        snapshotReady = false;
    }
    nds->OnVBlank = [this] { onVBlank(); };
}

bool ControlServer::isHalted()
{
    melonDS::NDS* nds = instance->getNDS();
    if (!nds) return false;
    EmuThread* thread = instance->getEmuThread();
    if (thread && !thread->emuIsRunning()) return true;
#ifdef GDBSTUB_ENABLED
    return nds->ARM9->GdbStub.IsHalted() || nds->ARM7->GdbStub.IsHalted();
#else
    return false;
#endif
}

bool ControlServer::snapshotForRequest(const std::string& source, Gpu3DSnapshot& out,
                                       std::string& err)
{
    const std::string mode = source.empty() ? "auto" : source;

    {
        std::lock_guard<std::mutex> lk(snapshotMutex);
        if (hasCachedSnapshot && mode != "fresh")
        {
            out = cachedSnapshot;
            return true;
        }
    }

    if (mode == "last")
    {
        err = "no cached 3D snapshot (use emu.step_frame(1, snapshot=true) or source=fresh)";
        return false;
    }

    if (isHalted())
    {
        err = "3D snapshot requires the emulator running; use "
              "emu.step_frame(1, snapshot=true) then source=last";
        return false;
    }

    if (!requestSnapshot(out, 2000))
    {
        err = "3D snapshot timeout (emulator paused?)";
        return false;
    }
    return true;
}

// Runs on the emu thread. Appends one GXFIFO entry (O(1)); the ring drops the
// oldest entries on overflow.
void ControlServer::onGXCommand(uint32_t cmd, uint32_t param, uint32_t pc,
                                uint32_t frame, uint32_t fifo)
{
    std::lock_guard<std::mutex> lk(gxMutex);
    if (!gxActive) return;
    gxRing.push({cmd, param, pc, frame, fifo});
}

// Disables the trace but leaves the collected entries readable via 3d.gxfifo.read.
void ControlServer::stopGXTrace()
{
    melonDS::NDS* nds = instance->getNDS();
    if (nds) nds->OnGXCommand = nullptr;

    std::lock_guard<std::mutex> lk(gxMutex);
    gxActive = false;
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
    auto t0 = std::chrono::steady_clock::now();
    bool ret = handleRequest(client, body);
    if (logTraffic)
    {
        json req = json::parse(body, nullptr, false);
        std::string method = req.is_object() ? req.value("method", "") : std::string("<bad>");
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Info, "[Control] %s dur=%lldus\n",
                      method.c_str(), (long long)us);
    }
    return ret;
}

bool ControlServer::handleRequest(Client* client, const std::string& body)
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
        if (logTraffic)
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "[Control] %s error %d: %s\n",
                          method.c_str(), code, msg.c_str());
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

    if (method == "emu.status")
    {
        if (!nds) return fail(-32000, "emulator not running");
        EmuThread* thread = instance->getEmuThread();
        const uint32_t frame = thread ? thread->emuFrameCounter() : nds->NumFrames;
        const bool halted = isHalted();

        json snapshot = nullptr;
        {
            std::lock_guard<std::mutex> lk(snapshotMutex);
            if (hasCachedSnapshot)
            {
                const uint32_t snapFrame = cachedSnapshot.frame;
                const uint32_t age = (frame >= snapFrame) ? (frame - snapFrame) : 0;
                snapshot = {{"frame", snapFrame}, {"age_frames", age}, {"stale", age > 0}};
            }
        }
        return ok({{"running", !halted}, {"halted", halted}, {"frame", frame},
                   {"snapshot", snapshot}});
    }
    if (method == "emu.step_frame")
    {
        int n = params.value("n", 1);
        if (n < 1) n = 1;
        const bool snapshot = params.value("snapshot", false);
        EmuThread* thread = instance->getEmuThread();
        if (snapshot) armStepSnapshot();
        thread->emuStepFrames(n, snapshot);
        lastStepFrame = nds ? nds->NumFrames : 0;

        json result = {{"ok", true}, {"frames", n}, {"frame", nds ? nds->NumFrames : 0}};
        if (snapshot)
        {
            std::lock_guard<std::mutex> lk(snapshotMutex);
            // a step that never hit a VBlank (e.g. reset mid-step) leaves it pending
            snapshotPending = false;
            if (hasCachedSnapshot && cachedSnapshot.valid)
                result["snapshot_frame"] = cachedSnapshot.frame;
            else
                result["snapshot_error"] = "no VBlank reached during the step";
        }
        return ok(result);
    }
    if (method == "emu.reset")
    {
        if (!nds) return fail(-32000, "emulator not running");
        instance->getEmuThread()->emuReset(false);
        lastStepFrame = 0;
        return ok({{"ok", true}});
    }
#ifdef GDBSTUB_ENABLED
    if (method.rfind("gdb.hits.", 0) == 0)
    {
        if (!nds) return fail(-32000, "emulator not running");
        const std::string core = params.value("core", std::string("arm9"));
        Gdb::GdbStub* stub = GetHitStub(nds, core);
        if (!stub) return fail(-32602, "core must be arm9 or arm7");

        if (method == "gdb.hits.start")
        {
            int cap = params.value("capacity", 4096);
            if (cap < 1) cap = 1;
            if (cap > 65536) cap = 65536;
            stub->StartHitLog((size_t)cap);
            return ok({{"ok", true}, {"capacity", cap}});
        }
        if (method == "gdb.hits.stop")
        {
            stub->StopHitLog();
            return ok({{"ok", true}});
        }
        if (method == "gdb.hits.clear")
        {
            stub->ClearHits();
            return ok({{"ok", true}});
        }
        if (method == "gdb.hits.read")
        {
            size_t dropped = 0;
            auto hits = stub->ReadHits(dropped);
            json arr = json::array();
            for (const auto& hit : hits)
            {
                const char* kind = "unknown";
                switch (hit.kind)
                {
                case 0: kind = "breakpoint"; break;
                case 2: kind = "write"; break;
                case 3: kind = "read"; break;
                case 4: kind = "access"; break;
                case 5: kind = "dma-write"; break;
                }
                arr.push_back({{"pc", hit.pc}, {"pc_fault", hit.pc_fault},
                                 {"addr", hit.addr}, {"width", hit.width},
                                 {"rmw", hit.rmw}, {"kind", kind}});
            }
            return ok({{"count", arr.size()}, {"dropped", dropped}, {"hits", arr}});
        }
        return fail(-32601, "unknown gdb.hits method");
    }
#endif

    if (method == "savestate.save" || method == "savestate.load")
    {
        if (!nds) return fail(-32000, "emulator not running");

        const bool isSave = (method == "savestate.save");
        const std::string name = params.value("name", std::string(""));
        const std::string owner = params.value("owner", std::string(""));
        const std::string label = params.value("label", std::string(""));
        const bool force = params.value("force", false);

        std::string path = params.value("path", std::string(""));
        if (!name.empty())
        {
            std::string safe;
            if (!SanitizeStateName(name, safe))
                return fail(-32602, "invalid savestate name (use [A-Za-z0-9._-], not numeric)");
            path = melonDS::Platform::GetLocalFilePath("savestate_named_" + safe + ".sav");
        }
        else if (path.empty())
        {
            int slot = params.value("slot", 0);
            path = melonDS::Platform::GetLocalFilePath("savestate_" + std::to_string(slot) + ".sav");
        }

        StateFileLock lock(path);
        if (!lock.ok())
            return fail(-32000, "savestate is locked by another writer");

        const std::string sidecarPath = StateSidecarPath(path);
        std::string gamecode;
        if (melonDS::NDSCart::CartCommon* cart = nds->GetNDSCart())
        {
            const melonDS::NDSHeader& header = cart->GetHeader();
            gamecode.assign(header.GameCode, sizeof(header.GameCode));
            while (!gamecode.empty() && (gamecode.back() == '\0' || gamecode.back() == ' '))
                gamecode.pop_back();
        }

        if (isSave)
        {
            const std::string tmp = path + ".tmp";
            EmuThread* thread = instance->getEmuThread();
            if (!thread->saveState(QString::fromStdString(tmp)))
            {
                std::error_code ec; std::filesystem::remove(tmp, ec);
                return fail(-32000, "savestate save failed");
            }

            uint32_t crc = 0;
            uint64_t size = 0;
            if (!StateFileDigest(tmp, crc, size))
            {
                std::error_code ec; std::filesystem::remove(tmp, ec);
                return fail(-32000, "savestate written but unreadable");
            }
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                std::error_code rm;
                std::filesystem::remove(tmp, rm);
                return fail(-32000, "savestate rename failed");
            }

            const uint32_t frame = thread->lastStateFrame();
            const uint32_t pc = thread->lastStatePC();
            const bool midFrame = isHalted() &&
                (lastStepFrame == 0 || nds->NumFrames != lastStepFrame);
            json meta = {
                {"version", 1}, {"path", path}, {"gamecode", gamecode},
                {"frame", frame}, {"pc", pc}, {"size", size}, {"crc32", crc},
                {"created_at", (long long)std::time(nullptr)},
                {"owner", owner}, {"label", label}, {"mid_frame", midFrame},
            };
            if (!WriteJsonAtomic(sidecarPath, meta))
                return fail(-32000, "savestate saved but sidecar write failed");

            json readback;
            if (!ReadJsonFile(sidecarPath, readback)
                || readback.value("crc32", 0u) != crc
                || readback.value("size", (uint64_t)0) != size)
                return fail(-32000, "savestate verification failed");

            json result = meta;
            result["ok"] = true;
            result["verified"] = true;
            if (midFrame)
                result["hint"] = "saved while halted mid-frame; use emu_step_frame to stop at a frame boundary";
            return ok(result);
        }

        // load: validate the file against its sidecar BEFORE touching the state
        json meta;
        const bool hasMeta = ReadJsonFile(sidecarPath, meta);
        uint32_t curCrc = 0;
        uint64_t curSize = 0;
        if (!StateFileDigest(path, curCrc, curSize))
            return fail(-32000, "savestate file missing or unreadable");

        if (hasMeta)
        {
            if (meta.value("size", (uint64_t)0) != curSize
                || meta.value("crc32", 0u) != curCrc)
                return fail(-32000, "savestate file does not match its sidecar (corrupted or clobbered)");
            const std::string metaGame = meta.value("gamecode", std::string(""));
            if (!metaGame.empty() && !gamecode.empty() && metaGame != gamecode)
                return fail(-32000, "savestate gamecode differs from the loaded ROM");
            const std::string metaOwner = meta.value("owner", std::string(""));
            if (!owner.empty() && !metaOwner.empty() && owner != metaOwner && !force)
                return fail(-32000, "savestate belongs to another owner (use force=true)");
        }

        EmuThread* thread = instance->getEmuThread();
        if (!thread->loadState(QString::fromStdString(path)))
            return fail(-32000, "savestate load failed");

        json result = {{"ok", true}, {"path", path}, {"verified", false}};
        if (hasMeta)
        {
            const uint32_t frame = thread->lastStateFrame();
            const uint32_t pc = thread->lastStatePC();
            const uint32_t metaFrame = meta.value("frame", 0u);
            const uint32_t metaPc = meta.value("pc", 0u);
            const bool frameOk = frame == metaFrame;
            const bool pcOk = metaPc == 0 || pc == metaPc;
            result["verified"] = frameOk && pcOk;
            result["frame"] = frame;
            result["pc"] = pc;
            if (!frameOk)
                result["divergence"] = {{"field", "frame"}, {"expected", metaFrame}, {"actual", frame}};
            else if (!pcOk)
                result["divergence"] = {{"field", "pc"}, {"expected", metaPc}, {"actual", pc}};
        }
        else
        {
            result["warning"] = "savestate has no sidecar; loaded unverified";
        }
        return ok(result);
    }

    if (method == "savestate.list")
    {
        const std::string probe =
            melonDS::Platform::GetLocalFilePath("savestate_probe.sav");
        const std::filesystem::path dir = std::filesystem::path(probe).parent_path();
        json states = json::array();
        try
        {
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                if (!entry.is_regular_file()) continue;
                const auto& p = entry.path();
                if (p.extension() != ".sav") continue;

                const std::string path = p.string();
                json item = {{"path", path}, {"name", p.stem().string()},
                             {"verified", false}, {"clobbered", false}};
                json meta;
                if (ReadJsonFile(StateSidecarPath(path), meta))
                {
                    for (auto it = meta.begin(); it != meta.end(); ++it)
                        item[it.key()] = it.value();
                    uint32_t crc = 0;
                    uint64_t size = 0;
                    if (StateFileDigest(path, crc, size)
                        && size == meta.value("size", (uint64_t)0)
                        && crc == meta.value("crc32", 0u))
                    {
                        item["verified"] = true;
                    }
                    else
                    {
                        item["clobbered"] = true;
                    }
                }
                else
                {
                    item["warning"] = "no sidecar";
                }
                states.push_back(item);
            }
        }
        catch (...)
        {
            return fail(-32000, "could not list savestates");
        }
        return ok({{"count", states.size()}, {"states", states}});
    }

    if (method == "rom.info")
    {
        melonDS::NDSCart::CartCommon* cart = nds ? nds->GetNDSCart() : nullptr;
        if (!cart)
        {
            std::lock_guard<std::mutex> lk(romMutex);
            checksumCart = nullptr;
            checksumName.clear();
            checksumLength = 0;
            checksumValue = 0;
            return ok({{"loaded", false}});
        }

        const std::string name = instance->getROMName();
        const uint32_t length = cart->GetROMLength();
        uint32_t checksum;
        {
            std::lock_guard<std::mutex> lk(romMutex);
            if (checksumCart != cart || checksumName != name || checksumLength != length)
            {
                checksumCart = cart;
                checksumName = name;
                checksumLength = length;
                checksumValue = cart->Checksum();
            }
            checksum = checksumValue;
        }

        const melonDS::NDSHeader& header = cart->GetHeader();
        return ok({{"loaded", true},
                   {"name", name},
                   {"path", instance->getROMPath()},
                   {"size", length},
                   {"gamecode", std::string(header.GameCode, 4)},
                   {"checksum", checksum}});
    }
    if (method == "rom.read")
    {
        melonDS::NDSCart::CartCommon* cart = nds ? nds->GetNDSCart() : nullptr;
        if (!cart) return fail(-32000, "no ROM loaded");

        const int offset = params.value("offset", -1);
        const int size = params.value("size", 0);
        if (offset < 0 || size <= 0)
            return fail(-32602, "offset must be >= 0 and size must be > 0");
        if (size > ROM_READ_MAX)
            return fail(-32602, "size exceeds the per-call limit");

        const uint64_t end = (uint64_t)offset + (uint64_t)size;
        if (end > (uint64_t)cart->GetROMLength())
            return fail(-32602, "read out of ROM bounds");

        std::vector<uint8_t> data((size_t)size);
        memcpy(data.data(), cart->GetROM() + offset, (size_t)size);

        if (!isNotification)
        {
            json result = {{"offset", offset}, {"size", size},
                           {"binary", {{"bytes", data.size()}}}};
            json resp = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            sendMessage(client, resp.dump());
            sendBinary(client, data);
        }
        return true;
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

    if (method == "3d.state")
    {
        Gpu3DSnapshot s;
        std::string snapErr;
        if (!snapshotForRequest(params.value("source", std::string("auto")), s, snapErr))
            return fail(-32000, snapErr);

        auto matrix = [](const int32_t* m) {
            json a = json::array();
            for (int i = 0; i < 16; i++) a.push_back(m[i]);
            return a;
        };
        json viewport = json::array();
        for (int i = 0; i < 6; i++) viewport.push_back(s.viewport[i]);

        return ok({
            {"frame", s.frame},
            {"enabled", {{"geometry", s.geometryEnabled}, {"rendering", s.renderingEnabled}}},
            {"disp3dcnt", s.dispCnt},
            {"gxstat", s.gxStat},
            {"fifo", {{"cmd", s.fifoCmd}, {"pipe", s.fifoPipe}, {"stall", s.fifoStall}}},
            {"matrix_mode", s.matrixMode},
            {"matrices", {{"projection", matrix(s.proj)}, {"position", matrix(s.pos)},
                          {"vector", matrix(s.vec)}, {"texture", matrix(s.tex)}}},
            {"viewport", viewport},
            {"fog", {{"color", s.fogColor}, {"offset", s.fogOffset}}},
            {"counts", {{"vertices", s.numVertices}, {"polygons", s.numPolygons},
                        {"render_polygons", s.renderPolygons}}},
        });
    }

    if (method == "3d.polygons")
    {
        Gpu3DSnapshot s;
        std::string snapErr;
        if (!snapshotForRequest(params.value("source", std::string("auto")), s, snapErr))
            return fail(-32000, snapErr);

        uint32_t maxItems = params.value("max_items", 512);
        if (maxItems < 1) maxItems = 1;
        if (maxItems > 8192) maxItems = 8192;
        bool withVerts = params.value("vertices", true);

        const uint32_t n = std::min<uint32_t>((uint32_t)s.polys.size(), maxItems);
        json polys = json::array();
        for (uint32_t i = 0; i < n; i++)
        {
            const Gpu3DPoly& p = s.polys[i];
            json pj = {
                {"attr", p.attr}, {"tex_param", p.texParam}, {"tex_palette", p.texPalette},
                {"type", p.type}, {"num_vertices", p.numVertices},
                {"translucent", p.translucent},
            };
            if (withVerts)
            {
                json verts = json::array();
                for (uint32_t v = 0; v < p.numVertices; v++)
                {
                    const Gpu3DVertex& vv = p.verts[v];
                    verts.push_back({
                        {"x", vv.x}, {"y", vv.y}, {"z", vv.z}, {"w", vv.w},
                        {"color", {vv.r, vv.g, vv.b}},
                        {"texcoord", {vv.u, vv.v}},
                    });
                }
                pj["vertices"] = verts;
            }
            polys.push_back(pj);
        }

        return ok({{"frame", s.frame}, {"count", polys.size()},
                   {"total", (int)s.polys.size()}, {"polygons", polys}});
    }

    if (method == "3d.textures")
    {
        Gpu3DSnapshot s;
        std::string snapErr;
        if (!snapshotForRequest(params.value("source", std::string("auto")), s, snapErr))
            return fail(-32000, snapErr);

        json list = json::array();
        for (size_t i = 0; i < s.textures.size(); i++)
        {
            const uint64_t key = s.textures[i];
            const uint32_t texParam = (uint32_t)(key >> 32);
            const uint32_t texPalette = (uint32_t)key;
            const uint32_t fmt = (texParam >> 26) & 0x7;
            const uint32_t w = 8u << ((texParam >> 20) & 0x7);
            const uint32_t h = 8u << ((texParam >> 23) & 0x7);
            list.push_back({
                {"index", (int)i}, {"tex_param", texParam}, {"tex_palette", texPalette},
                {"format", fmt}, {"width", w}, {"height", h},
                {"vram_addr", (texParam & 0xFFFF) * 8},
            });
        }
        return ok({{"frame", s.frame}, {"count", list.size()}, {"textures", list}});
    }

    if (method == "3d.texture")
    {
        if (!nds) return fail(-32000, "emulator not running");

        uint32_t texParam = params.value("tex_param", 0u);
        uint32_t texPalette = params.value("tex_palette", 0u);
        if (params.contains("index"))
        {
            int index = params.value("index", 0);
            Gpu3DSnapshot s;
            std::string snapErr;
            if (!snapshotForRequest(params.value("source", std::string("auto")), s, snapErr))
                return fail(-32000, snapErr);
            if (index < 0 || index >= (int)s.textures.size())
                return fail(-32602, "texture index out of range");
            const uint64_t key = s.textures[index];
            texParam = (uint32_t)(key >> 32);
            texPalette = (uint32_t)key;
        }

        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        instance->getEmuThread()->emuPause(false);
        const bool decoded = melonDS::Decode3DTexture(*nds, texParam, texPalette, rgba, w, h);
        instance->getEmuThread()->emuUnpause(false);

        if (!decoded)
            return fail(-32000, "texture decode failed (no texture/unsupported format)");

        if (!isNotification)
        {
            json result = {
                {"binary", {{"bytes", rgba.size()}, {"width", w}, {"height", h},
                            {"format", "rgba8"}}},
                {"tex_param", texParam}, {"tex_palette", texPalette},
            };
            json resp = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            sendMessage(client, resp.dump());
            sendBinary(client, rgba);
        }
        return true;
    }

    if (method == "3d.texture_raw")
    {
        if (!nds) return fail(-32000, "emulator not running");

        uint32_t texParam = params.value("tex_param", 0u);
        uint32_t texPalette = params.value("tex_palette", 0u);
        if (params.contains("index"))
        {
            int index = params.value("index", 0);
            Gpu3DSnapshot s;
            std::string snapErr;
            if (!snapshotForRequest(params.value("source", std::string("auto")), s, snapErr))
                return fail(-32000, snapErr);
            if (index < 0 || index >= (int)s.textures.size())
                return fail(-32602, "texture index out of range");
            const uint64_t key = s.textures[index];
            texParam = (uint32_t)(key >> 32);
            texPalette = (uint32_t)key;
        }

        const uint32_t fmt = (texParam >> 26) & 0x7;
        const uint32_t w = 8u << ((texParam >> 20) & 0x7);
        const uint32_t h = 8u << ((texParam >> 23) & 0x7);
        uint32_t size = 0;
        switch (fmt)
        {
        case 7: size = w * h * 2; break;
        case 1: case 6: size = w * h; break;
        case 2: size = w * h / 4; break;
        case 3: size = w * h / 2; break;
        case 4: size = w * h; break;
        default: return fail(-32000, "raw texture unsupported for this format");
        }

        const uint32_t addr = (texParam & 0xFFFF) * 8;
        if (addr + size > sizeof(nds->GPU.VRAMFlat_Texture))
            return fail(-32000, "texture range outside VRAM");

        std::vector<uint8_t> raw;
        instance->getEmuThread()->emuPause(false);
        {
            auto dirty = nds->GPU.VRAMDirty_Texture.DeriveState(nds->GPU.VRAMMap_Texture, nds->GPU);
            nds->GPU.MakeVRAMFlat_TextureCoherent(dirty);
            raw.assign(nds->GPU.VRAMFlat_Texture + addr,
                       nds->GPU.VRAMFlat_Texture + addr + size);
        }
        instance->getEmuThread()->emuUnpause(false);

        if (!isNotification)
        {
            json result = {
                {"binary", {{"bytes", raw.size()}, {"width", w}, {"height", h},
                            {"format", "raw"}}},
                {"tex_param", texParam}, {"tex_palette", texPalette},
                {"format_id", fmt}, {"size", size},
            };
            json resp = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            sendMessage(client, resp.dump());
            sendBinary(client, raw);
        }
        return true;
    }

    if (method == "3d.gxfifo.start")
    {
        if (!nds) return fail(-32000, "emulator not running");
        uint32_t maxItems = params.value("max_items", 65536);
        if (maxItems < 1) maxItems = 1;
        if (maxItems > (1u << 22)) maxItems = 1u << 22;

        {
            std::lock_guard<std::mutex> lk(gxMutex);
            gxActive = true;
            gxRing.reset(maxItems);
        }
        nds->OnGXCommand = [this](uint32_t cmd, uint32_t param, uint32_t pc,
                                  uint32_t frame, uint32_t fifo) {
            onGXCommand(cmd, param, pc, frame, fifo);
        };
        return ok({{"ok", true}, {"max_items", maxItems}});
    }
    if (method == "3d.gxfifo.read")
    {
        std::vector<GXEvent> batch;
        uint64_t dropped;
        {
            std::lock_guard<std::mutex> lk(gxMutex);
            gxRing.drain(batch);
            dropped = gxRing.dropped;
            gxRing.dropped = 0;
        }
        json cmds = json::array();
        for (size_t i = 0; i < batch.size(); )
        {
            const GXEvent& first = batch[i];
            const uint32_t cmd = first.cmd & 0xFF;
            const uint32_t total = melonDS::CmdNumParams[cmd];
            json params = json::array();
            size_t j = i;
            uint32_t consumed = 0;

            while (j < batch.size() && (batch[j].cmd & 0xFF) == cmd && consumed < total)
            {
                params.push_back(batch[j].param);
                consumed++;
                j++;
            }
            if (total == 0 || j == i)
                j = i + 1;

            cmds.push_back({{"cmd", cmd}, {"name", GXCommandName(cmd)},
                            {"pc", first.pc}, {"frame", first.frame},
                            {"fifo", first.fifo}, {"params", params}});
            i = j;
        }
        return ok({{"count", cmds.size()}, {"entries", batch.size()},
                   {"dropped", dropped}, {"commands", cmds}});
    }
    if (method == "3d.gxfifo.stop")
    {
        stopGXTrace();
        return ok({{"ok", true}});
    }

    if (method == "origin.start")
    {
        if (!nds) return fail(-32000, "emulator not running");
        uint32_t maxItems = params.value("max_items", 65536);
        if (maxItems < 1) maxItems = 1;
        if (maxItems > (1u << 22)) maxItems = 1u << 22;

        {
            std::lock_guard<std::mutex> lk(originMutex);
            originActive = true;
            originRing.reset(maxItems);
        }
        nds->OnDmaTransfer = [this](uint32_t cpu, uint32_t ch, uint32_t src, uint32_t dst,
                                    uint32_t cnt, uint32_t pc) {
            onDmaTransfer(cpu, ch, src, dst, cnt, pc);
        };
        return ok({{"ok", true}, {"max_items", maxItems}});
    }
    if (method == "origin.stop")
    {
        {
            std::lock_guard<std::mutex> lk(originMutex);
            originActive = false;
        }
        bool dmaOn;
        {
            std::lock_guard<std::mutex> lk(dmaMutex);
            dmaOn = dmaActive;
        }
        if (nds && !dmaOn) nds->OnDmaTransfer = nullptr;
        return ok({{"ok", true}});
    }
    if (method == "origin.query" || method == "gxfifo.origin")
    {
        const bool gx = method == "gxfifo.origin";
        uint32_t addr = gx ? 0x04000400 : params.value("addr", 0u);
        uint32_t len = gx ? 4 : params.value("len", 1u);
        if (!len) len = 1;
        int maxAge = params.value("max_age_frames", 0);
        int maxHits = params.value("max_hits", 16);
        if (maxHits < 1) maxHits = 1;
        if (maxHits > 256) maxHits = 256;

        const uint32_t currentFrame = nds ? nds->NumFrames : 0;
        json hits = json::array();
        uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> lk(originMutex);
            dropped = originRing.dropped;
            const size_t cap = originRing.buf.size();
            const uint32_t endAddr = addr + len;
            if (cap > 0)
            {
                for (size_t i = 0; i < originRing.count && (int)hits.size() < maxHits; i++)
                {
                    const size_t idx = (originRing.head + cap - 1 - i) % cap;
                    const OriginEvent& e = originRing.buf[idx];
                    if (e.dstLo >= endAddr || e.dstHi <= addr) continue;
                    if (maxAge > 0 && e.frame + (uint32_t)maxAge < currentFrame) continue;
                    hits.push_back({{"src", e.src}, {"dst_lo", e.dstLo}, {"dst_hi", e.dstHi},
                                    {"len", e.len}, {"pc", e.pc}, {"frame", e.frame},
                                    {"kind", e.kind}});
                }
            }
        }
        return ok({{"count", hits.size()}, {"unknown", hits.empty()},
                   {"hits", hits}, {"dropped", dropped}});
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
void ControlServer::onVBlank() {}
bool ControlServer::requestSnapshot(Gpu3DSnapshot&, int) { return false; }
bool ControlServer::snapshotForRequest(const std::string&, Gpu3DSnapshot&, std::string& err)
{
    err = "control channel not supported on this platform";
    return false;
}
void ControlServer::armStepSnapshot() {}
bool ControlServer::isHalted() { return false; }
void ControlServer::onGXCommand(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void ControlServer::stopGXTrace() {}

#endif // _WIN32
