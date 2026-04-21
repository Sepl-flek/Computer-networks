#include <iostream>
#include <vector>
#include <queue>
#include <fstream>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include "message_ex.h"

#define PORT 54000
#define THREADS 5

struct Client {
    int sock;
    std::string nick;
    bool auth;
};

std::vector<Client> clients;
std::queue<int> q;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

uint32_t global_id = 1;

void log_recv() {
    std::cout << "[Transport] recv() via TCP\n";
    std::cout << "[Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP\n";
    std::cout << "[Network Access] frame received\n";
}

void log_send() {
    std::cout << "[Transport] send() via TCP\n";
    std::cout << "[Internet] sending packet\n";
    std::cout << "[Network Access] frame sent\n";
}

void save_json(MessageEx &m, bool delivered, bool offline) {
    std::ofstream f("history.json", std::ios::app);
    f << "{\n"
      << "  \"msg_id\": " << m.msg_id << ",\n"
      << "  \"timestamp\": " << m.timestamp << ",\n"
      << "  \"sender\": \"" << m.sender << "\",\n"
      << "  \"receiver\": \"" << m.receiver << "\",\n"
      << "  \"type\": " << (int)m.type << ",\n"
      << "  \"text\": \"" << m.payload << "\",\n"
      << "  \"delivered\": " << (delivered?"true":"false") << ",\n"
      << "  \"is_offline\": " << (offline?"true":"false") << "\n"
      << "}\n";
}

Client* find_client(const std::string &nick) {
    for (auto &c : clients)
        if (c.nick == nick) return &c;
    return nullptr;
}

void send_msg(int sock, MessageEx &m) {
    log_send();
    send(sock, &m, sizeof(m), 0);
}

void broadcast(MessageEx &m, int sender) {
    for (auto &c : clients)
        if (c.sock != sender && c.auth)
            send_msg(c.sock, m);
}

