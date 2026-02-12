#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    char buffer[1024];

    while (true) {
        std::string message;
        std::cin >> message;

        if (message == "exit")
            break;

        sendto(
            sock,
            message.c_str(),
            message.size(),
            0,
            (sockaddr*)&serverAddr,
            sizeof(serverAddr)
        );

        int n = recvfrom(
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            nullptr,
            nullptr
        );

        if (n == SOCKET_ERROR)
            continue;

        buffer[n] = '\0';
        std::cout << "Server: " << buffer << std::endl;
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
