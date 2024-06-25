// Copyright (c) 2019-present Anonymous275.
// BeamMP Launcher code is not in the public domain and is not free software.
// One must be granted explicit permission by the copyright holder in order to modify or distribute any part of the source or binaries.
// Anything else is prohibited. Modified works may not be published and have be upstreamed to the official repository.
///
/// Created by Anonymous275 on 7/25/2020
///
#include "Helpers.h"
#include "Network/network.hpp"
#include "NetworkHelpers.h"
#include <algorithm>
#include <span>
#include <vector>
#include <zlib.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__linux__)
#include "linuxfixes.h"
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "Logger.h"
#include <charconv>
#include <mutex>
#include <string>
#include <thread>

std::chrono::time_point<std::chrono::high_resolution_clock> PingStart, PingEnd;
bool GConnected = false;
bool CServer = true;
SOCKET CSocket = -1;
SOCKET GSocket = -1;

int KillSocket(uint64_t Dead) {
    if (Dead == (SOCKET)-1) {
        debug("Kill socket got -1 returning...");
        return 0;
    }
    shutdown(Dead, SD_BOTH);
    int a = closesocket(Dead);
    if (a != 0) {
        warn("Failed to close socket!");
    }
    return a;
}

bool CheckBytes(uint32_t Bytes) {
    if (Bytes == 0) {
        debug("(Proxy) Connection closing");
        return false;
    } else if (Bytes < 0) {
        debug("(Proxy) send failed with error: " + std::to_string(WSAGetLastError()));
        return false;
    }
    return true;
}

void GameSend(std::string_view RawData) {
    static std::mutex Lock;
    std::scoped_lock Guard(Lock);
    if (TCPTerminate || !GConnected || CSocket == -1)
        return;
    int32_t Size, Temp, Sent;
    uint32_t DataSize = RawData.size();
    std::vector<char> Data(sizeof(DataSize) + RawData.size());
    std::copy_n(reinterpret_cast<char*>(&DataSize), sizeof(DataSize), Data.begin());
    std::copy_n(RawData.data(), RawData.size(), Data.begin() + sizeof(DataSize));
    Size = Data.size();
    Sent = 0;
#ifdef DEBUG
    if (Size > 1000) {
        debug("Launcher -> game (" + std::to_string(Size) + ")");
    }
#endif
    do {
        if (Sent > -1) {
            Temp = send(CSocket, &Data[Sent], Size - Sent, 0);
        }
        if (!CheckBytes(Temp))
            return;
        Sent += Temp;
    } while (Sent < Size);
    // send separately to avoid an allocation for += "\n"
    /*Temp = send(CSocket, "\n", 1, 0);
    if (!CheckBytes(Temp)) {
        return;
    }*/
}

void ServerSend(const std::vector<char>& Data, bool Rel) {
    if (Terminate || Data.empty())
        return;
    char C = 0;
    bool Ack = false;
    int DLen = int(Data.size());
    if (DLen > 3)
        C = Data.at(0);
    if (C == 'O' || C == 'T')
        Ack = true;
    if (C == 'N' || C == 'W' || C == 'Y' || C == 'V' || C == 'E' || C == 'C')
        Rel = true;
    if (compressBound(Data.size()) > 1024)
        Rel = true;
    if (Ack || Rel) {
        if (Ack || DLen > 1000)
            SendLarge(Data);
        else
            TCPSend(Data, TCPSock);
    } else
        UDPSend(Data);
}

void NetReset() {
    TCPTerminate = false;
    GConnected = false;
    Terminate = false;
    UlStatus = "Ulstart";
    MStatus = " ";
    if (UDPSock != (SOCKET)(-1)) {
        debug("Terminating UDP Socket : " + std::to_string(TCPSock));
        KillSocket(UDPSock);
    }
    UDPSock = -1;
    if (TCPSock != (SOCKET)(-1)) {
        debug("Terminating TCP Socket : " + std::to_string(TCPSock));
        KillSocket(TCPSock);
    }
    TCPSock = -1;
    if (GSocket != (SOCKET)(-1)) {
        debug("Terminating GTCP Socket : " + std::to_string(GSocket));
        KillSocket(GSocket);
    }
    GSocket = -1;
}

SOCKET SetupListener() {
    if (GSocket != -1)
        return GSocket;
    struct addrinfo* result = nullptr;
    struct addrinfo hints { };
    int iRes;
#ifdef _WIN32
    WSADATA wsaData;
    iRes = WSAStartup(514, &wsaData); // 2.2
    if (iRes != 0) {
        error("(Proxy) WSAStartup failed with error: " + std::to_string(iRes));
        return -1;
    }
#endif

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    iRes = getaddrinfo(nullptr, std::to_string(DEFAULT_PORT + 1).c_str(), &hints, &result);
    if (iRes != 0) {
        error("(Proxy) info failed with error: " + std::to_string(iRes));
        WSACleanup();
    }
    GSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (GSocket == -1) {
        error("(Proxy) socket failed with error: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(result);
        WSACleanup();
        return -1;
    }
#if defined (__linux__)
    int opt = 1;
    if (setsockopt(GSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        error("setsockopt(SO_REUSEADDR) failed");
#endif
    iRes = bind(GSocket, result->ai_addr, (int)result->ai_addrlen);
    if (iRes == SOCKET_ERROR) {
        error("(Proxy) bind failed with error: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(result);
        KillSocket(GSocket);
        WSACleanup();
        return -1;
    }
    freeaddrinfo(result);
    iRes = listen(GSocket, SOMAXCONN);
    if (iRes == SOCKET_ERROR) {
        error("(Proxy) listen failed with error: " + std::to_string(WSAGetLastError()));
        KillSocket(GSocket);
        WSACleanup();
        return -1;
    }
    return GSocket;
}
void AutoPing() {
    while (!Terminate) {
        ServerSend(strtovec("p"), false);
        PingStart = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
int ClientID = -1;
void ParserAsync(std::string_view Data) {
    if (Data.empty())
        return;
    char Code = Data.at(0), SubCode = 0;
    if (Data.length() > 1)
        SubCode = Data.at(1);
    switch (Code) {
    case 'p':
        PingEnd = std::chrono::high_resolution_clock::now();
        if (PingStart > PingEnd)
            ping = 0;
        else
            ping = int(std::chrono::duration_cast<std::chrono::milliseconds>(PingEnd - PingStart).count());
        return;
    case 'M':
        MStatus = Data;
        UlStatus = "Uldone";
        return;
    default:
        break;
    }
    GameSend(Data);
}
void ServerParser(std::string_view Data) {
    ParserAsync(Data);
}
void NetMain(const std::string& IP, int Port) {
    std::thread Ping(AutoPing);
    Ping.detach();
    UDPClientMain(IP, Port);
    CServer = true;
    Terminate = true;
    info("Connection Terminated!");
}
void TCPGameServer(const std::string& IP, int Port) {
    GSocket = SetupListener();
    while (!TCPTerminate && GSocket != -1) {
        debug("MAIN LOOP OF GAME SERVER");
        GConnected = false;
        if (!CServer) {
            warn("Connection still alive terminating");
            NetReset();
            TCPTerminate = true;
            Terminate = true;
            break;
        }
        if (CServer) {
            std::thread Client(TCPClientMain, IP, Port);
            Client.detach();
        }
        CSocket = accept(GSocket, nullptr, nullptr);
        if (CSocket == -1) {
            debug("(Proxy) accept failed with error: " + std::to_string(WSAGetLastError()));
            break;
        }
        debug("(Proxy) Game Connected!");
        GConnected = true;
        if (CServer) {
            std::thread t1(NetMain, IP, Port);
            t1.detach();
            CServer = false;
        }
        std::vector<char> data {};

        // Read byte by byte until '>' is rcved then get the size and read based on it
        do {
            try {
                ReceiveFromGame(CSocket, data);
                ServerSend(data, false);
            } catch (const std::exception& e) {
                error(std::string("Error while receiving from game: ") + e.what());
                break;
            }
        } while (!TCPTerminate);
    }
    TCPTerminate = true;
    GConnected = false;
    Terminate = true;
    if (CSocket != SOCKET_ERROR)
        KillSocket(CSocket);
    debug("END OF GAME SERVER");
}
