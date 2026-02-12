#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket error\n";
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, (sockaddr*)&serverAddr, sizeof(serverAddr));

    std::cout << "UDP Echo Server started\n";

    char buffer[1024];
    sockaddr_in clientAddr{};
    int clientLen = sizeof(clientAddr);

    while (true) {
        int received = recvfrom(
            sock,
            buffer,
            sizeof(buffer) - 1,
            0,
            (sockaddr*)&clientAddr,
            &clientLen
        );

        if (received == SOCKET_ERROR) continue;

        buffer[received] = '\0';
        std::cout << "Received: " << buffer << std::endl;

        sendto(
            sock,
            buffer,
            received,
            0,
            (sockaddr*)&clientAddr,
            clientLen
        );
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
