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

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

ControlServer::ControlServer(EmuInstance* inst) : instance(inst), listenFd(-1), running(false)
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
        // read headers until the blank line
        size_t contentLength = 0;
        bool haveLength = false;
        std::string line;
        while (true)
        {
            if (!ReadLine(fd, line)) goto done;
            if (line.empty()) break;
            const std::string key = "Content-Length:";
            if (line.compare(0, key.size(), key) == 0)
            {
                contentLength = strtoul(line.c_str() + key.size(), nullptr, 10);
                haveLength = true;
            }
        }

        if (!haveLength) goto done;

        {
            std::string body;
            body.resize(contentLength);
            if (!ReadExact(fd, &body[0], contentLength)) goto done;
            if (!handleMessage(client, body)) goto done;
        }
    }

done:
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

void ControlServer::sendMessage(Client* client, const std::string& body)
{
    std::string out = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < out.size())
    {
        ssize_t n = send(client->fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
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

    return fail(-32601, "method not found");
}

#else // _WIN32

bool ControlServer::start(const std::string&) { return false; }
void ControlServer::stop() {}
void ControlServer::acceptLoop() {}
void ControlServer::clientLoop(Client*) {}
bool ControlServer::handleMessage(Client*, const std::string&) { return false; }
void ControlServer::sendMessage(Client*, const std::string&) {}

#endif // _WIN32
