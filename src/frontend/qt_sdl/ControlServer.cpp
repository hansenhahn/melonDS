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
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#endif

ControlServer::ControlServer(EmuInstance* inst) : instance(inst), listenFd(-1), running(false),
    captureRequested(false), captureDone(false), captureValid(false), captureScreens(2)
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

        struct pollfd pfd {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0)
        {
            // streaming: time for a sample
            if (client->streaming) sendStreamSample(client);
            continue;
        }
        if (!(pfd.revents & POLLIN)) break;

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
    if (nds && nds->GPU.GetFramebuffers(&top, &bottom))
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

#endif // _WIN32
