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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EmuInstance;

// Control channel for external agents (MCP). Runs on its own thread and never
// touches the emulation core directly: input is applied by the emu thread from
// shared fields, and core operations are queued to the emu thread.
class ControlServer
{
public:
    explicit ControlServer(EmuInstance* inst);
    ~ControlServer();

    bool start(const std::string& path);
    void stop();

private:
    struct Client
    {
        int fd;
    };

    void acceptLoop();
    void clientLoop(Client* client);

    // Returns true if the client should be kept alive.
    bool handleMessage(Client* client, const std::string& body);
    void sendMessage(Client* client, const std::string& body);

    EmuInstance* instance;

    int listenFd;
    std::string socketPath;
    std::thread acceptThread;
    std::atomic<bool> running;

    std::mutex clientsMutex;
    std::vector<Client*> clients;
};

#endif
