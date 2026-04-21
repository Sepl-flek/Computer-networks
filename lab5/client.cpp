#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include "message_ex.h"

#define PORT 54000

int sock;

void* recv_thread(void*) {
    MessageEx msg{};
    while (true) {
        int b = recv(sock, &msg, sizeof(msg), 0);
        if (b <= 0) break;

        std::cout << "[" << msg.msg_id << "] " << msg.sender << ": " << msg.payload << std::endl;
    }
    return nullptr;
}

int main() {
    sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    connect(sock, (sockaddr*)&addr, sizeof(addr));

    std::string nick;
    std::cout << "Enter nick: ";
    std::getline(std::cin, nick);

    MessageEx msg{};
    msg.type = 7;
    strcpy(msg.sender, nick.c_str());
    send(sock, &msg, sizeof(msg), 0);

    pthread_t t;
    pthread_create(&t, nullptr, recv_thread, nullptr);

    while (true) {
        std::string input;
        std::getline(std::cin, input);

        MessageEx m{};
        strcpy(m.sender, nick.c_str());

        if (input == "/quit") break;

        else if (input.rfind("/w ",0)==0) {
            m.type = 8;
            auto pos = input.find(' ',3);
            std::string to = input.substr(3,pos-3);
            std::string text = input.substr(pos+1);
            strcpy(m.receiver,to.c_str());
            strcpy(m.payload,text.c_str());
        }
        else {
            m.type = 3;
            strcpy(m.payload,input.c_str());
        }

        send(sock,&m,sizeof(m),0);
    }

    close(sock);
}
