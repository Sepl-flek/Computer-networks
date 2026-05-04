#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <pthread.h>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "message.h"

#define PORT 8080
#define THREAD_POOL_SIZE 10
#define MAX_QUEUE_SIZE 100
#define MAX_HISTORY 100
#define LOG_FILE "messages_log.json"

bool keepRunning = true;
std::vector<ClientInfo> clients;
std::vector<OfflineMsg> offline;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t json_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
std::queue<int> client_queue;

struct NetworkSimulator {
    int delay_ms = 0;
    double drop_rate = 0.0;
    double corrupt_rate = 0.0;
};

NetworkSimulator g_sim;
std::mt19937 g_rng(std::random_device{}());
pthread_mutex_t rng_mutex = PTHREAD_MUTEX_INITIALIZER;

std::string format_double(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << value;
    return oss.str();
}

void log_layer(const std::string& layer, const std::string& message) {
    std::cout << "[" << layer << "] " << message << std::endl;
}

void handleSignal(int) {
    keepRunning = false;
    pthread_cond_broadcast(&queue_cond);
}

std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

std::string unescape_json(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += s[i + 1]; break;
            }
            ++i;
        } else {
            result += s[i];
        }
    }
    return result;
}

std::string extract_string(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":\"";
    size_t start = line.find(pattern);
    if (start == std::string::npos) return "";
    start += pattern.length();
    size_t i = start;
    while (i < line.size()) {
        if (line[i] == '"' && line[i - 1] != '\\') break;
        ++i;
    }
    return unescape_json(line.substr(start, i - start));
}

long extract_number(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    size_t start = line.find(pattern);
    if (start == std::string::npos) return 0;
    start += pattern.length();
    size_t end = line.find_first_of(",}", start);
    return std::stol(line.substr(start, end - start));
}

bool extract_bool(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    size_t start = line.find(pattern);
    if (start == std::string::npos) return false;
    start += pattern.length();
    return line.compare(start, 4, "true") == 0;
}

MessageEx ntoh_message(const MessageEx& net_msg) {
    MessageEx host_msg = net_msg;
    host_msg.length = ntohl(net_msg.length);
    host_msg.msg_id = ntohl(net_msg.msg_id);
    return host_msg;
}

MessageEx hton_message(const MessageEx& host_msg) {
    MessageEx net_msg = host_msg;
    net_msg.length = htonl(host_msg.length);
    net_msg.msg_id = htonl(host_msg.msg_id);
    return net_msg;
}

bool send_message(int socket_fd, const MessageEx& msg) {
    log_layer("Transport", "send() - transmitting data");
    MessageEx net_msg = hton_message(msg);
    return send(socket_fd, &net_msg, sizeof(MessageEx), 0) >= 0;
}

bool recv_message(int socket_fd, MessageEx& msg) {
    log_layer("Transport", "recv() - receiving data");
    if (recv(socket_fd, &msg, sizeof(MessageEx), 0) <= 0) return false;
    msg = ntoh_message(msg);
    return true;
}

double random_unit() {
    pthread_mutex_lock(&rng_mutex);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double value = dist(g_rng);
    pthread_mutex_unlock(&rng_mutex);
    return value;
}

int random_index(int max_exclusive) {
    pthread_mutex_lock(&rng_mutex);
    std::uniform_int_distribution<int> dist(0, max_exclusive - 1);
    int value = dist(g_rng);
    pthread_mutex_unlock(&rng_mutex);
    return value;
}

void log_message_to_json(const MessageEx& msg, bool delivered, bool is_offline) {
    pthread_mutex_lock(&json_mutex);
    std::ofstream file(LOG_FILE, std::ios::app);
    if (file.is_open()) {
        file << "{";
        file << "\"msg_id\":" << msg.msg_id << ",";
        file << "\"timestamp\":" << msg.timestamp << ",";
        file << "\"sender\":\"" << escape_json(msg.sender) << "\",";
        file << "\"receiver\":\"" << escape_json(msg.receiver) << "\",";
        file << "\"type\":\"" << (msg.type == MSG_PRIVATE ? "MSG_PRIVATE" : "MSG_TEXT") << "\",";
        file << "\"text\":\"" << escape_json(msg.payload) << "\",";
        file << "\"delivered\":" << (delivered ? "true" : "false") << ",";
        file << "\"is_offline\":" << (is_offline ? "true" : "false");
        file << "}\n";
    }
    pthread_mutex_unlock(&json_mutex);
}

bool update_delivered(uint32_t msg_id) {
    pthread_mutex_lock(&json_mutex);
    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    bool updated = false;
    while (std::getline(file, line)) {
        if (extract_number(line, "msg_id") == static_cast<long>(msg_id)) {
            size_t pos = line.find("\"delivered\":false");
            if (pos != std::string::npos) {
                line.replace(pos, strlen("\"delivered\":false"), "\"delivered\":true");
                updated = true;
            }
        }
        lines.push_back(line);
    }
    file.close();

    std::ofstream out(LOG_FILE, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
    pthread_mutex_unlock(&json_mutex);
    return updated;
}

std::string format_timestamp(time_t timestamp) {
    struct tm* tm_info = localtime(&timestamp);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

std::string get_history(int n, const std::string& requesting_user) {
    std::vector<HistoryMessage> all_messages;
    pthread_mutex_lock(&json_mutex);
    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return "No history available.\n";
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        HistoryMessage msg;
        msg.msg_id = static_cast<uint32_t>(extract_number(line, "msg_id"));
        msg.timestamp = static_cast<time_t>(extract_number(line, "timestamp"));
        msg.sender = extract_string(line, "sender");
        msg.receiver = extract_string(line, "receiver");
        msg.type = extract_string(line, "type");
        msg.text = extract_string(line, "text");
        msg.delivered = extract_bool(line, "delivered");
        msg.is_offline = extract_bool(line, "is_offline");
        if (msg.delivered) {
            all_messages.push_back(msg);
        }
    }
    pthread_mutex_unlock(&json_mutex);

    std::ostringstream result;
    int total = static_cast<int>(all_messages.size());
    int start = (n > 0 && n < total) ? total - n : 0;

    for (int i = start; i < total; ++i) {
        const auto& msg = all_messages[i];
        bool is_private = msg.type == "MSG_PRIVATE";
        bool is_for_requester = (msg.receiver == requesting_user || msg.sender == requesting_user);

        result << "[" << format_timestamp(msg.timestamp) << "]";
        result << "[id=" << msg.msg_id << "]";
        if (msg.is_offline) {
            if (is_for_requester) {
                result << "[OFFLINE][" << msg.sender << " -> " << msg.receiver << "]: " << msg.text;
            } else {
                result << "[OFFLINE][" << msg.sender << " -> " << msg.receiver << "]: "
                       << std::string(msg.text.length(), '*');
            }
        } else if (is_private) {
            if (is_for_requester) {
                result << "[PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: " << msg.text;
            } else {
                result << "[PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: "
                       << std::string(msg.text.length(), '*');
            }
        } else {
            result << "[" << msg.sender << "]: " << msg.text;
        }
        result << "\n";
    }

    if (result.str().empty()) return "No messages in history.\n";
    return result.str();
}

ClientInfo* find_client_by_socket(int socket_fd) {
    for (auto& client : clients) {
        if (client.socket == socket_fd) return &client;
    }
    return nullptr;
}

ClientInfo* find_client_by_nickname(const std::string& nickname) {
    for (auto& client : clients) {
        if (client.authenticated && std::string(client.nickname) == nickname) return &client;
    }
    return nullptr;
}

bool is_nickname_unique(const std::string& nickname) {
    for (const auto& client : clients) {
        if (client.authenticated && std::string(client.nickname) == nickname) return false;
    }
    return true;
}

std::string get_online_users_list() {
    std::string result = "[SERVER]: Online users\n";
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.authenticated && client.active && strlen(client.nickname) > 0) {
            result += "  " + std::string(client.nickname) + "\n";
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return result;
}

int parse_history_param(const char* payload) {
    if (payload == nullptr || payload[0] == '\0') return -1;
    char* endptr = nullptr;
    long val = strtol(payload, &endptr, 10);
    if (endptr == payload || *endptr != '\0' || val <= 0) return 0;
    if (val > MAX_HISTORY) val = MAX_HISTORY;
    return static_cast<int>(val);
}

void remember_msg_id(ClientInfo& client, uint32_t msg_id) {
    client.last_ids[client.last_ids_pos] = msg_id;
    client.last_ids_pos = (client.last_ids_pos + 1) % client.last_ids.size();
    if (client.last_ids_count < client.last_ids.size()) {
        client.last_ids_count++;
    }
}

bool is_duplicate_message(ClientInfo& client, uint32_t msg_id) {
    for (size_t i = 0; i < client.last_ids_count; ++i) {
        if (client.last_ids[i] == msg_id) return true;
    }
    return false;
}

bool should_ack(uint8_t type) {
    return type == MSG_TEXT || type == MSG_PRIVATE || type == MSG_PING;
}

void send_ack(int client_sock, uint32_t msg_id) {
    MessageEx ack{};
    ack.type = MSG_ACK;
    ack.msg_id = msg_id;
    ack.timestamp = time(nullptr);
    log_layer("Transport][ACK", "send MSG_ACK (id=" + std::to_string(msg_id) + ")");
    send_message(client_sock, ack);
}

void remove_client(int socket_fd) {
    pthread_mutex_lock(&clients_mutex);
    auto it = std::find_if(clients.begin(), clients.end(),
        [socket_fd](const ClientInfo& client) { return client.socket == socket_fd; });

    if (it != clients.end()) {
        if (it->authenticated) {
            MessageEx info_msg{};
            info_msg.type = MSG_SERVER_INFO;
            snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] disconnected", it->nickname);
            info_msg.length = strlen(info_msg.payload) + 1;
            info_msg.timestamp = time(nullptr);
            for (const auto& client : clients) {
                if (client.socket != socket_fd && client.authenticated) {
                    send_message(client.socket, info_msg);
                }
            }
            std::cout << "User [" << it->nickname << "] disconnected" << std::endl;
        }
        close(it->socket);
        clients.erase(it);
    }

    pthread_mutex_unlock(&clients_mutex);
}

void broadcast_message(const MessageEx& msg, int sender_socket) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.socket != sender_socket && client.authenticated) {
            send_message(client.socket, msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_private_message(const MessageEx& incoming_msg, int sender_socket, const std::string& sender_nickname) {
    std::string target_nick = incoming_msg.receiver;
    std::string message = incoming_msg.payload;

    pthread_mutex_lock(&clients_mutex);
    ClientInfo* target = find_client_by_nickname(target_nick);
    pthread_mutex_unlock(&clients_mutex);

    MessageEx private_msg = incoming_msg;
    snprintf(private_msg.sender, MAX_NAME, "%s", sender_nickname.c_str());
    snprintf(private_msg.receiver, MAX_NAME, "%s", target_nick.c_str());
    private_msg.timestamp = time(nullptr);

    if (!target) {
        OfflineMsg offline_msg{};
        snprintf(offline_msg.text, MAX_PAYLOAD, "%s", private_msg.payload);
        snprintf(offline_msg.sender, MAX_NAME, "%s", private_msg.sender);
        snprintf(offline_msg.receiver, MAX_NAME, "%s", private_msg.receiver);
        offline_msg.timestamp = private_msg.timestamp;
        offline_msg.msg_id = private_msg.msg_id;

        pthread_mutex_lock(&offline_mutex);
        offline.push_back(offline_msg);
        pthread_mutex_unlock(&offline_mutex);

        log_message_to_json(private_msg, false, true);
    } else if (!send_message(target->socket, private_msg)) {
        MessageEx error_msg{};
        snprintf(error_msg.payload, MAX_PAYLOAD, "Failed to send message to '%s'", target_nick.c_str());
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        error_msg.timestamp = time(nullptr);
        send_message(sender_socket, error_msg);
    } else {
        std::cout << "[PRIVATE] " << sender_nickname << " -> " << target_nick << ": " << message << std::endl;
        log_message_to_json(private_msg, true, false);
    }
}

bool authenticate_client(int client_sock, const std::string& nickname) {
    if (nickname.empty()) {
        MessageEx error_msg{};
        strcpy(error_msg.payload, "Nickname cannot be empty");
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        send_message(client_sock, error_msg);
        return false;
    }

    if (!is_nickname_unique(nickname)) {
        MessageEx error_msg{};
        snprintf(error_msg.payload, MAX_PAYLOAD, "Nickname '%s' is already taken", nickname.c_str());
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        send_message(client_sock, error_msg);
        return false;
    }

    ClientInfo* client = find_client_by_socket(client_sock);
    if (!client) return false;

    strncpy(client->nickname, nickname.c_str(), MAX_NAME - 1);
    client->nickname[MAX_NAME - 1] = '\0';
    client->authenticated = true;

    MessageEx welcome_msg{};
    snprintf(welcome_msg.payload, MAX_PAYLOAD, "Welcome %s", nickname.c_str());
    welcome_msg.length = strlen(welcome_msg.payload) + 1;
    welcome_msg.type = MSG_WELCOME;
    welcome_msg.timestamp = time(nullptr);
    send_message(client_sock, welcome_msg);

    MessageEx info_msg{};
    snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] connected", nickname.c_str());
    info_msg.length = strlen(info_msg.payload) + 1;
    info_msg.type = MSG_SERVER_INFO;
    info_msg.timestamp = time(nullptr);
    for (const auto& c : clients) {
        if (c.socket != client_sock && c.authenticated) {
            send_message(c.socket, info_msg);
        }
    }

    std::cout << "User [" << nickname << "] connected" << std::endl;
    return true;
}

bool apply_network_simulation(MessageEx& msg) {
    if (g_sim.delay_ms > 0) {
        log_layer("Transport][SIM", "DELAY applied: " + std::to_string(g_sim.delay_ms) + " ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(g_sim.delay_ms));
    }

    if (g_sim.drop_rate > 0.0 && random_unit() < g_sim.drop_rate) {
        log_layer("Transport][SIM", "DROP (id=" + std::to_string(msg.msg_id) +
            ", rate=" + format_double(g_sim.drop_rate) + ")");
        return false;
    }

    if (g_sim.corrupt_rate > 0.0 && msg.length > 0 && msg.payload[0] != '\0' && random_unit() < g_sim.corrupt_rate) {
        size_t length = strnlen(msg.payload, MAX_PAYLOAD);
        int index = random_index(static_cast<int>(length));
        msg.payload[index] = static_cast<char>(msg.payload[index] ^ 0x01);
        log_layer("Transport][SIM", "CORRUPT payload (id=" + std::to_string(msg.msg_id) + ")");
    }

    return true;
}

void deliver_offline_messages(ClientInfo* auth_client) {
    pthread_mutex_lock(&offline_mutex);
    for (auto it = offline.begin(); it != offline.end(); ) {
        if (strcmp(it->receiver, auth_client->nickname) == 0) {
            MessageEx off_msg{};
            snprintf(off_msg.payload, MAX_PAYLOAD, "[OFFLINE]%s", it->text);
            off_msg.length = strlen(off_msg.payload) + 1;
            off_msg.type = MSG_PRIVATE;
            snprintf(off_msg.sender, MAX_NAME, "%s", it->sender);
            snprintf(off_msg.receiver, MAX_NAME, "%s", it->receiver);
            off_msg.timestamp = it->timestamp;
            off_msg.msg_id = it->msg_id;

            if (send_message(auth_client->socket, off_msg)) {
                update_delivered(it->msg_id);
                it = offline.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }
    pthread_mutex_unlock(&offline_mutex);
}

void* handle_client(int client_sock) {
    MessageEx msg{};
    ClientInfo client;
    client.socket = client_sock;
    client.authenticated = false;
    client.active = true;

    socklen_t addr_len = sizeof(client.address);
    getpeername(client_sock, reinterpret_cast<struct sockaddr*>(&client.address), &addr_len);

    pthread_mutex_lock(&clients_mutex);
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);

    if (!recv_message(client_sock, msg) || msg.type != MSG_AUTH) {
        MessageEx error_msg{};
        strcpy(error_msg.payload, "Authentication required");
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        send_message(client_sock, error_msg);
        remove_client(client_sock);
        return nullptr;
    }

    if (!authenticate_client(client_sock, msg.payload)) {
        remove_client(client_sock);
        return nullptr;
    }

    pthread_mutex_lock(&clients_mutex);
    ClientInfo* auth_client = find_client_by_socket(client_sock);
    pthread_mutex_unlock(&clients_mutex);
    if (!auth_client) {
        remove_client(client_sock);
        return nullptr;
    }

    deliver_offline_messages(auth_client);

    while (keepRunning) {
        if (!recv_message(client_sock, msg)) {
            log_layer("Application", "client disconnected");
            break;
        }

        log_layer("Transport", "incoming type=" + std::to_string(msg.type) + " id=" + std::to_string(msg.msg_id));

        if (!apply_network_simulation(msg)) {
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        ClientInfo* sender = find_client_by_socket(client_sock);
        if (!sender) {
            pthread_mutex_unlock(&clients_mutex);
            break;
        }

        bool duplicate = should_ack(msg.type) && is_duplicate_message(*sender, msg.msg_id);
        if (duplicate) {
            pthread_mutex_unlock(&clients_mutex);
            log_layer("Application][DEDUP", "duplicate ignored (id=" + std::to_string(msg.msg_id) + ")");
            send_ack(client_sock, msg.msg_id);
            continue;
        }

        if (should_ack(msg.type)) {
            remember_msg_id(*sender, msg.msg_id);
        }

        std::string sender_nickname = sender->nickname;
        pthread_mutex_unlock(&clients_mutex);

        switch (msg.type) {
            case MSG_TEXT: {
                log_layer("Application][ACK", "process MSG_TEXT (id=" + std::to_string(msg.msg_id) + ")");
                std::cout << "[" << sender_nickname << "]: " << msg.payload << std::endl;

                MessageEx broadcast_msg = msg;
                snprintf(broadcast_msg.sender, MAX_NAME, "%s", sender_nickname.c_str());
                broadcast_msg.receiver[0] = '\0';
                broadcast_msg.timestamp = time(nullptr);
                broadcast_message(broadcast_msg, client_sock);
                log_message_to_json(broadcast_msg, true, false);
                send_ack(client_sock, msg.msg_id);
                break;
            }
            case MSG_PRIVATE:
                log_layer("Application][ACK", "process MSG_PRIVATE (id=" + std::to_string(msg.msg_id) + ")");
                send_private_message(msg, client_sock, sender_nickname);
                send_ack(client_sock, msg.msg_id);
                break;
            case MSG_LIST: {
                MessageEx info_msg{};
                std::string user_list = get_online_users_list();
                snprintf(info_msg.payload, MAX_PAYLOAD, "%s", user_list.c_str());
                info_msg.length = strlen(info_msg.payload) + 1;
                info_msg.type = MSG_SERVER_INFO;
                info_msg.timestamp = time(nullptr);
                send_message(client_sock, info_msg);
                break;
            }
            case MSG_HISTORY: {
                int n = parse_history_param(msg.payload);
                if (n == 0) {
                    MessageEx error_msg{};
                    snprintf(error_msg.payload, MAX_PAYLOAD, "Invalid parameter. Use /history or /history <positive_number>");
                    error_msg.length = strlen(error_msg.payload) + 1;
                    error_msg.type = MSG_ERROR;
                    send_message(client_sock, error_msg);
                    break;
                }

                MessageEx history_msg{};
                std::string history = get_history(n, sender_nickname);
                snprintf(history_msg.payload, MAX_PAYLOAD, "%s", history.c_str());
                history_msg.length = strlen(history_msg.payload) + 1;
                history_msg.type = MSG_HISTORY_DATA;
                history_msg.timestamp = time(nullptr);
                send_message(client_sock, history_msg);
                break;
            }
            case MSG_PING: {
                log_layer("Transport][PING", "recv MSG_PING (id=" + std::to_string(msg.msg_id) + ")");
                MessageEx pong_msg{};
                strcpy(pong_msg.payload, "pong");
                pong_msg.length = strlen(pong_msg.payload) + 1;
                pong_msg.type = MSG_PONG;
                pong_msg.msg_id = msg.msg_id;
                pong_msg.timestamp = time(nullptr);
                log_layer("Transport][PING", "send MSG_PONG (id=" + std::to_string(msg.msg_id) + ")");
                send_message(client_sock, pong_msg);
                send_ack(client_sock, msg.msg_id);
                break;
            }
            case MSG_BYE:
                log_layer("Application", "client requested disconnect");
                goto disconnect_client;
            default: {
                MessageEx error_msg{};
                strcpy(error_msg.payload, "unknown message type");
                error_msg.length = strlen(error_msg.payload) + 1;
                error_msg.type = MSG_ERROR;
                send_message(client_sock, error_msg);
                break;
            }
        }
    }

disconnect_client:
    remove_client(client_sock);
    return nullptr;
}

void* worker_thread(void*) {
    while (keepRunning) {
        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty() && keepRunning) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        if (!keepRunning || client_queue.empty()) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        int client_sock = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);
        handle_client(client_sock);
    }
    return nullptr;
}

void add_to_queue(int client_sock) {
    pthread_mutex_lock(&queue_mutex);
    if (client_queue.size() < MAX_QUEUE_SIZE) {
        client_queue.push(client_sock);
        pthread_cond_signal(&queue_cond);
    } else {
        std::cout << "Queue is full, rejecting client" << std::endl;
        close(client_sock);
    }
    pthread_mutex_unlock(&queue_mutex);
}

void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--delay=", 0) == 0) {
            g_sim.delay_ms = std::max(0, std::stoi(arg.substr(8)));
        } else if (arg.rfind("--drop=", 0) == 0) {
            g_sim.drop_rate = std::clamp(std::stod(arg.substr(7)), 0.0, 1.0);
        } else if (arg.rfind("--corrupt=", 0) == 0) {
            g_sim.corrupt_rate = std::clamp(std::stod(arg.substr(10)), 0.0, 1.0);
        }
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    parse_args(argc, argv);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, 10) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }

    std::cout << "Server listening on port " << PORT << std::endl;
    std::cout << "Simulation: delay=" << g_sim.delay_ms
              << "ms drop=" << format_double(g_sim.drop_rate)
              << " corrupt=" << format_double(g_sim.corrupt_rate) << std::endl;

    pthread_t thread_pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        if (pthread_create(&thread_pool[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            close(server_sock);
            return 1;
        }
        pthread_detach(thread_pool[i]);
    }

    while (keepRunning) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_sock < 0) {
            if (keepRunning) perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        log_layer("Internet", "new connection from " + std::string(client_ip) + ":" +
            std::to_string(ntohs(client_addr.sin_port)));
        add_to_queue(client_sock);
    }

    close(server_sock);

    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) {
        close(client.socket);
    }
    clients.clear();
    pthread_mutex_unlock(&clients_mutex);

    std::cout << "Server shutting down" << std::endl;
    return 0;
}
