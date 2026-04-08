#include <iostream>
#include <vector>
#include <queue>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "message.h"

#define PORT 54000
#define THREAD_POOL_SIZE 10

struct Client
{
    int sock;
    std::string nickname;
    bool authenticated;
};

std::queue<int> client_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

std::vector<Client> clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/////////////////////////////////////////
// 🔹 OSI LOGGING
/////////////////////////////////////////

void log_recv()
{
    std::cout << "[Layer 4 - Transport] recv()\n";
}

void log_parse(int type)
{
    std::cout << "[Layer 6 - Presentation] parsed type = " << type << "\n";
}

void log_session(bool auth)
{
    std::cout << "[Layer 5 - Session] authenticated = " << auth << "\n";
}

void log_app(const std::string& msg)
{
    std::cout << "[Layer 7 - Application] " << msg << "\n";
}

/////////////////////////////////////////
// 🔹 UTILS
/////////////////////////////////////////

Client* find_by_nick(const std::string& nick)
{
    for (auto& c : clients)
        if (c.nickname == nick)
            return &c;
    return nullptr;
}

bool nickname_exists(const std::string& nick)
{
    return find_by_nick(nick) != nullptr;
}

void send_message(int sock, Message& msg)
{
    log_app("prepare response");
    std::cout << "[Layer 6 - Presentation] serialize Message\n";
    std::cout << "[Layer 4 - Transport] send()\n";

    send(sock, &msg, sizeof(msg), 0);
}

void broadcast(const std::string& text, int sender)
{
    pthread_mutex_lock(&clients_mutex);

    Message msg{};
    msg.type = MSG_TEXT;
    strcpy(msg.payload, text.c_str());

    for (auto& c : clients)
    {
        if (c.sock != sender && c.authenticated)
            send_message(c.sock, msg);
    }

    pthread_mutex_unlock(&clients_mutex);
}

void send_to(Client& target, const std::string& text)
{
    Message msg{};
    msg.type = MSG_PRIVATE;
    strcpy(msg.payload, text.c_str());

    send_message(target.sock, msg);
}

void remove_client(int sock)
{
    pthread_mutex_lock(&clients_mutex);

    clients.erase(
        std::remove_if(clients.begin(), clients.end(),
                       [sock](const Client& c){ return c.sock == sock; }),
        clients.end()
    );

    pthread_mutex_unlock(&clients_mutex);
}

/////////////////////////////////////////
// 🔹 WORKER
/////////////////////////////////////////

void* worker(void*)
{
    while (true)
    {
        int client_sock;

        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty())
            pthread_cond_wait(&queue_cond, &queue_mutex);

        client_sock = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);

        Client client{};
        client.sock = client_sock;
        client.authenticated = false;

        Message msg{};

        // === AUTH ===
        log_recv();
        int bytes = recv(client_sock, &msg, sizeof(msg), 0);

        if (bytes <= 0)
        {
            close(client_sock);
            continue;
        }

        log_parse(msg.type);

        if (msg.type != MSG_AUTH)
        {
            close(client_sock);
            continue;
        }

        std::string nick = msg.payload;

        if (nick.empty() || nickname_exists(nick))
        {
            Message err{};
            err.type = MSG_ERROR;
            strcpy(err.payload, "Invalid or duplicate nickname");
            send_message(client_sock, err);
            close(client_sock);
            continue;
        }

        client.nickname = nick;
        client.authenticated = true;

        log_session(true);

        pthread_mutex_lock(&clients_mutex);
        clients.push_back(client);
        pthread_mutex_unlock(&clients_mutex);

        std::cout << "User [" << nick << "] connected\n";

        broadcast("User [" + nick + "] connected", client_sock);

        // === LOOP ===
        while (true)
        {
            log_recv();
            bytes = recv(client_sock, &msg, sizeof(msg), 0);

            if (bytes <= 0)
                break;

            log_parse(msg.type);
            log_session(client.authenticated);

            if (!client.authenticated)
                continue;

            if (msg.type == MSG_TEXT)
            {
                std::string text = "[" + client.nickname + "]: " + msg.payload;

                log_app("broadcast message");
                broadcast(text, client_sock);
            }

            else if (msg.type == MSG_PRIVATE)
            {
                std::string data = msg.payload;

                auto pos = data.find(':');
                if (pos == std::string::npos)
                    continue;

                std::string target = data.substr(0, pos);
                std::string message = data.substr(pos + 1);

                Client* receiver = find_by_nick(target);

                if (!receiver)
                {
                    Message err{};
                    err.type = MSG_ERROR;
                    strcpy(err.payload, "User not found");
                    send_message(client_sock, err);
                    continue;
                }

                std::string text = "[PRIVATE][" + client.nickname + "]: " + message;

                log_app("private message");
                send_to(*receiver, text);
            }

            else if (msg.type == MSG_PING)
            {
                Message pong{};
                pong.type = MSG_PONG;
                send_message(client_sock, pong);
            }

            else if (msg.type == MSG_BYE)
            {
                break;
            }
        }

        std::cout << "User [" << client.nickname << "] disconnected\n";

        broadcast("User [" + client.nickname + "] disconnected", client_sock);

        remove_client(client_sock);
        close(client_sock);
    }

    return nullptr;
}

/////////////////////////////////////////
// 🔹 MAIN
/////////////////////////////////////////

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);

    std::cout << "Server started\n";

    pthread_t threads[THREAD_POOL_SIZE];

    for (int i = 0; i < THREAD_POOL_SIZE; i++)
    {
        pthread_create(&threads[i], nullptr, worker, nullptr);
        pthread_detach(threads[i]);
    }

    while (true)
    {
        int client = accept(server_fd, nullptr, nullptr);

        pthread_mutex_lock(&queue_mutex);
        client_queue.push(client);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }

    return 0;
}