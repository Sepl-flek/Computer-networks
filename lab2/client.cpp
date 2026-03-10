#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include "message.h"

#define PORT 54000

int main()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    connect(sock, (sockaddr*)&addr, sizeof(addr));

    std::cout << "Connected\n";

    Message msg{};

    std::string name;
    std::cout << "Enter nickname: ";
    std::getline(std::cin, name);

    msg.type = MSG_HELLO;
    strcpy(msg.payload, name.c_str());
    msg.length = sizeof(msg.type) + name.size();

    send(sock, &msg, sizeof(msg), 0);

    recv(sock, &msg, sizeof(msg), 0);

    if (msg.type == MSG_WELCOME)
    {
        std::cout << msg.payload << std::endl;
    }

    while (true)
    {
        std::string input;
        std::cout << "> ";
        std::getline(std::cin, input);

        Message out{};

        if (input == "/ping")
        {
            out.type = MSG_PING;
            out.length = sizeof(out.type);
        }
        else if (input == "/quit")
        {
            out.type = MSG_BYE;
            out.length = sizeof(out.type);
            send(sock, &out, sizeof(out), 0);
            break;
        }
        else
        {
            out.type = MSG_TEXT;
            strcpy(out.payload, input.c_str());
            out.length = sizeof(out.type) + input.size();
        }

        send(sock, &out, sizeof(out), 0);

        if (out.type == MSG_PING)
        {
            recv(sock, &msg, sizeof(msg), 0);

            if (msg.type == MSG_PONG)
            {
                std::cout << "PONG\n";
            }
        }
    }

    close(sock);

    std::cout << "Disconnected\n";

    return 0;
}