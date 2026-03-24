#include <iostream>
#include <vector>
#include <queue>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "message.h"

#define PORT 54000
#define THREAD_POOL_SIZE 10

std::queue<int> client_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

std::vector<int> clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void broadcast(const Message& msg, int sender)
{
    pthread_mutex_lock(&clients_mutex);

    for (int client : clients)
    {
        if (client != sender)
        {
            send(client, &msg, sizeof(msg), 0);
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(int sock)
{
    pthread_mutex_lock(&clients_mutex);

    clients.erase(
        std::remove(clients.begin(), clients.end(), sock),
        clients.end()
    );

    pthread_mutex_unlock(&clients_mutex);
}

void* worker(void*)
{
    while (true)
    {
        int client;

        pthread_mutex_lock(&queue_mutex);

        while (client_queue.empty())
            pthread_cond_wait(&queue_cond, &queue_mutex);

        client = client_queue.front();
        client_queue.pop();

        pthread_mutex_unlock(&queue_mutex);


        Message msg{};

        int bytes = recv(client, &msg, sizeof(msg), 0);
        if (bytes <= 0)
        {
            close(client);
            continue;
        }

        if (msg.type != MSG_HELLO)
        {
            close(client);
            continue;
        }

        std::string name = msg.payload;
        std::cout << "Connected: " << name << std::endl;

        pthread_mutex_lock(&clients_mutex);
        clients.push_back(client);
        pthread_mutex_unlock(&clients_mutex);
        Message reply{};
        reply.type = MSG_WELCOME;
        strcpy(reply.payload, "Welcome!");
        reply.length = strlen(reply.payload);

        send(client, &reply, sizeof(reply), 0);
        while (true)
        {
            bytes = recv(client, &msg, sizeof(msg), 0);

            if (bytes <= 0)
            {
                std::cout << "Disconnected: " << name << std::endl;
                break;
            }

            if (msg.type == MSG_TEXT)
            {
                std::cout << name << ": " << msg.payload << std::endl;

                broadcast(msg, client);
            }

            else if (msg.type == MSG_PING)
            {
                Message pong{};
                pong.type = MSG_PONG;
                pong.length = 0;

                send(client, &pong, sizeof(pong), 0);
            }

            else if (msg.type == MSG_BYE)
            {
                std::cout << "Disconnected: " << name << std::endl;
                break;
            }
        }

        remove_client(client);
        close(client);
    }

    return nullptr;
}

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

    close(server_fd);
    return 0;
}