#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include "message.h"

#define PORT 54000

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 1);

    std::cout << "Server started\n";

    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);

    int client = accept(server_fd, (sockaddr*)&client_addr, &len);

    std::cout << "Client connected\n";

    Message msg{};

    recv(client, &msg, sizeof(msg), 0);

    if (msg.type == MSG_HELLO)
    {
        std::cout << "[client]: " << msg.payload << std::endl;

        Message reply{};
        reply.type = MSG_WELCOME;
        strcpy(reply.payload, "Welcome!");
        reply.length = sizeof(reply.type) + strlen(reply.payload);

        send(client, &reply, sizeof(reply), 0);
    }

    while (true)
    {
        int bytes = recv(client, &msg, sizeof(msg), 0);

        if (bytes == 0)
        {
            std::cout << "Client disconnected\n";
            break;
        }

        if (msg.type == MSG_TEXT)
        {
            std::cout << "[client]: " << msg.payload << std::endl;
        }

        if (msg.type == MSG_PING)
        {
            Message pong{};
            pong.type = MSG_PONG;
            pong.length = sizeof(pong.type);

            send(client, &pong, sizeof(pong), 0);
        }

        if (msg.type == MSG_BYE)
        {
            std::cout << "Client disconnected\n";
            break;
        }
    }

    close(client);
    close(server_fd);

    return 0;
}