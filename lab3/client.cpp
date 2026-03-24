#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "message.h"

#define PORT 54000

int sock;
std::string nickname;
bool running = true;

void* receiver(void*)
{
    Message msg{};

    while (running)
    {
        int bytes = recv(sock, &msg, sizeof(msg), 0);

        if (bytes <= 0)
        {
            std::cout << "Disconnected from server...\n";
            running = false;
            break;
        }

        if (msg.type == MSG_TEXT)
        {
            std::cout << msg.payload << std::endl;
        }

        if (msg.type == MSG_PONG)
        {
            std::cout << "PONG\n";
        }
    }

    return nullptr;
}

bool connect_to_server()
{
    sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0)
        return false;

    Message msg{};
    msg.type = MSG_HELLO;
    strcpy(msg.payload, nickname.c_str());
    msg.length = nickname.size();

    send(sock, &msg, sizeof(msg), 0);

    recv(sock, &msg, sizeof(msg), 0);

    if (msg.type == MSG_WELCOME)
    {
        std::cout << msg.payload << std::endl;
    }

    return true;
}

int main()
{
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nickname);

    while (true)
    {
        if (!connect_to_server())
        {
            std::cout << "Reconnect in 2 sec...\n";
            sleep(2);
            continue;
        }

        std::cout << "Connected\n";

        running = true;

        pthread_t recv_thread;
        pthread_create(&recv_thread, nullptr, receiver, nullptr);

        while (running)
        {
            std::string input;
            std::getline(std::cin, input);

            Message msg{};

            if (input == "/ping")
            {
                msg.type = MSG_PING;
            }
            else if (input == "/quit")
            {
                msg.type = MSG_BYE;
                send(sock, &msg, sizeof(msg), 0);
                running = false;
                break;
            }
            else
            {
                msg.type = MSG_TEXT;
                strcpy(msg.payload, input.c_str());
            }

            send(sock, &msg, sizeof(msg), 0);
        }

        close(sock);
        pthread_cancel(recv_thread);

        std::cout << "Reconnecting...\n";
        sleep(2);
    }

    return 0;
}