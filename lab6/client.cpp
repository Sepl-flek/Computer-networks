#include <arpa/inet.h>
#include <csignal>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cctype>
#include "message.h"

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define RECONNECT_DELAY 2
#define INPUT_TIMEOUT 1
#define ACK_TIMEOUT_MS 2000
#define MAX_RETRIES 3
#define DEFAULT_PING_COUNT 10

bool keepRunning = true;
bool connected = false;
int sock = -1;
std::string g_nickname;

pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
std::mutex pending_mutex;
std::condition_variable pending_cv;
std::map<uint32_t, PendingMsg> pending_messages;

std::mutex ping_mutex;
std::condition_variable ping_cv;
std::map<uint32_t, long long> ping_send_times;
std::map<uint32_t, double> ping_rtts;

std::mutex diag_mutex;
NetDiagMetrics last_diag;
std::vector<double> last_rtts;
std::vector<double> last_jitters;

std::atomic<uint32_t> next_msg_id{1};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string format_double(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << value;
    return oss.str();
}

std::string format_timestamp(time_t timestamp) {
    struct tm* tm_info = localtime(&timestamp);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

void handleSignal(int) {
    keepRunning = false;
    pending_cv.notify_all();
    ping_cv.notify_all();
}

bool read_input_with_timeout(char* buffer, int max_len, int timeout_seconds) {
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;
    int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
    if (result > 0) {
        if (fgets(buffer, max_len, stdin) != NULL) {
            buffer[strcspn(buffer, "\n")] = 0;
            return true;
        }
    }
    return false;
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

bool send_message_raw(int socket_fd, const MessageEx& msg) {
    MessageEx net_msg = hton_message(msg);
    return send(socket_fd, &net_msg, sizeof(MessageEx), 0) >= 0;
}

bool recv_message(int socket_fd, MessageEx& msg) {
    if (recv(socket_fd, &msg, sizeof(MessageEx), 0) <= 0) return false;
    msg = ntoh_message(msg);
    return true;
}

uint32_t generate_msg_id() {
    return next_msg_id.fetch_add(1);
}

bool send_over_socket(const MessageEx& msg) {
    pthread_mutex_lock(&sock_mutex);
    int current_sock = sock;
    bool ok = current_sock >= 0 && send_message_raw(current_sock, msg);
    pthread_mutex_unlock(&sock_mutex);
    return ok;
}

void drop_all_pending_as_failed() {
    std::lock_guard<std::mutex> lock(pending_mutex);
    for (auto& [id, pending] : pending_messages) {
        pending.failed = true;
    }
    pending_cv.notify_all();
}

void clear_ping_state() {
    std::lock_guard<std::mutex> lock(ping_mutex);
    ping_send_times.clear();
    ping_rtts.clear();
    ping_cv.notify_all();
}

void persist_netdiag(const NetDiagMetrics& metrics) {
    std::ofstream file("net_diag_" + g_nickname + ".json", std::ios::trunc);
    if (!file.is_open()) {
        std::cout << "[Application] failed to write net diagnostics file" << std::endl;
        return;
    }

    file << "{\n";
    file << "  \"nickname\": \"" << g_nickname << "\",\n";
    file << "  \"requested\": " << metrics.requested << ",\n";
    file << "  \"received\": " << metrics.received << ",\n";
    file << "  \"rtt_avg_ms\": " << std::fixed << std::setprecision(1) << metrics.avg_rtt_ms << ",\n";
    file << "  \"jitter_avg_ms\": " << std::fixed << std::setprecision(1) << metrics.avg_jitter_ms << ",\n";
    file << "  \"loss_percent\": " << std::fixed << std::setprecision(1) << metrics.loss_percent << "\n";
    file << "}\n";
}

void print_netdiag() {
    std::lock_guard<std::mutex> lock(diag_mutex);
    if (last_diag.requested == 0) {
        std::cout << "No diagnostics data available. Run /ping first." << std::endl;
        return;
    }

    std::cout << "RTT avg : " << format_double(last_diag.avg_rtt_ms) << " ms" << std::endl;
    std::cout << "Jitter  : " << format_double(last_diag.avg_jitter_ms) << " ms" << std::endl;
    std::cout << "Loss    : " << format_double(last_diag.loss_percent) << "%" << std::endl;
}

void update_last_diag(int requested, const std::vector<double>& rtts, const std::vector<double>& jitters) {
    NetDiagMetrics metrics;
    metrics.requested = requested;
    metrics.received = static_cast<int>(rtts.size());

    if (!rtts.empty()) {
        double sum = 0.0;
        for (double rtt : rtts) sum += rtt;
        metrics.avg_rtt_ms = sum / rtts.size();
    }

    if (!jitters.empty()) {
        double sum = 0.0;
        for (double jitter : jitters) sum += jitter;
        metrics.avg_jitter_ms = sum / jitters.size();
    }

    if (requested > 0) {
        metrics.loss_percent = (static_cast<double>(requested - metrics.received) / requested) * 100.0;
    }

    {
        std::lock_guard<std::mutex> lock(diag_mutex);
        last_diag = metrics;
        last_rtts = rtts;
        last_jitters = jitters;
    }

    persist_netdiag(metrics);
}

bool send_reliable_message(MessageEx& msg) {
    msg.msg_id = generate_msg_id();
    msg.timestamp = time(nullptr);

    PendingMsg pending{};
    pending.msg = msg;
    pending.send_time_ms = now_ms();
    pending.retries = 0;
    pending.acked = false;
    pending.failed = false;

    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_messages[msg.msg_id] = pending;
    }

    if (msg.type == MSG_PING) {
        std::lock_guard<std::mutex> lock(ping_mutex);
        ping_send_times[msg.msg_id] = pending.send_time_ms;
    }

    std::cout << "[Transport][RETRY] send ";
    if (msg.type == MSG_TEXT) std::cout << "MSG_TEXT";
    else if (msg.type == MSG_PRIVATE) std::cout << "MSG_PRIVATE";
    else std::cout << "MSG_PING";
    std::cout << " (id=" << msg.msg_id << ")" << std::endl;

    if (!send_over_socket(msg)) {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_messages[msg.msg_id].failed = true;
        pending_cv.notify_all();
        return false;
    }

    return true;
}

void handle_ack(uint32_t msg_id) {
    std::lock_guard<std::mutex> lock(pending_mutex);
    auto it = pending_messages.find(msg_id);
    if (it != pending_messages.end()) {
        it->second.acked = true;
        std::cout << "[Transport][ACK] ACK received (id=" << msg_id << ")" << std::endl;
        pending_cv.notify_all();
    }
}

void handle_pong(uint32_t msg_id) {
    long long sent_ms = -1;
    {
        std::lock_guard<std::mutex> lock(ping_mutex);
        auto it = ping_send_times.find(msg_id);
        if (it != ping_send_times.end()) {
            sent_ms = it->second;
            ping_send_times.erase(it);
        }
    }

    if (sent_ms >= 0) {
        double rtt = static_cast<double>(now_ms() - sent_ms);
        {
            std::lock_guard<std::mutex> lock(ping_mutex);
            ping_rtts[msg_id] = rtt;
        }
        std::cout << "[Transport][PING] recv MSG_PONG (id=" << msg_id << ", rtt="
                  << format_double(rtt) << "ms)" << std::endl;
        ping_cv.notify_all();
    }
}

void* retry_thread(void*) {
    while (keepRunning) {
        std::vector<MessageEx> to_resend;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            long long current_ms = now_ms();
            for (auto& [id, pending] : pending_messages) {
                if (pending.acked || pending.failed) continue;
                if (current_ms - pending.send_time_ms < ACK_TIMEOUT_MS) continue;

                std::cout << "[Transport][RETRY] wait ACK timeout" << std::endl;
                if (pending.retries >= MAX_RETRIES) {
                    pending.failed = true;
                    std::cout << "[Transport][RETRY] delivery failed (id=" << id << ")" << std::endl;
                    continue;
                }

                pending.retries++;
                pending.send_time_ms = current_ms;
                if (pending.msg.type == MSG_PING) {
                    std::lock_guard<std::mutex> ping_lock(ping_mutex);
                    ping_send_times[id] = current_ms;
                }
                to_resend.push_back(pending.msg);
                std::cout << "[Transport][RETRY] resend " << pending.retries
                          << "/" << MAX_RETRIES << " (id=" << id << ")" << std::endl;
            }
        }

        for (const auto& msg : to_resend) {
            send_over_socket(msg);
        }

        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            for (auto it = pending_messages.begin(); it != pending_messages.end(); ) {
                if (it->second.acked || it->second.failed) it = pending_messages.erase(it);
                else ++it;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return nullptr;
}

void* receive_thread(void*) {
    MessageEx msg;
    while (keepRunning && connected) {
        pthread_mutex_lock(&sock_mutex);
        int current_sock = sock;
        pthread_mutex_unlock(&sock_mutex);

        if (current_sock < 0) break;
        if (!recv_message(current_sock, msg)) {
            std::cout << "\nConnection to server lost" << std::endl;
            pthread_mutex_lock(&sock_mutex);
            connected = false;
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            drop_all_pending_as_failed();
            clear_ping_state();
            break;
        }

        switch (msg.type) {
            case MSG_WELCOME:
                std::cout << "\n*** " << msg.payload << " ***" << std::endl;
                break;
            case MSG_TEXT:
                std::cout << "[" << format_timestamp(msg.timestamp) << "]"
                          << "\n[id=" << msg.msg_id << "]"
                          << "[" << msg.sender << "]: " << msg.payload << std::endl;
                break;
            case MSG_PRIVATE:
                std::cout << "\n[PRIVATE]"
                          << "[" << format_timestamp(msg.timestamp) << "]"
                          << "[id=" << msg.msg_id << "]"
                          << "[" << msg.sender << "->" << msg.receiver << "]: "
                          << msg.payload << std::endl;
                break;
            case MSG_SERVER_INFO:
                std::cout << "\n[SERVER]: " << msg.payload << std::endl;
                break;
            case MSG_ERROR:
                std::cout << "\n[ERROR]: " << msg.payload << std::endl;
                break;
            case MSG_PONG:
                handle_pong(msg.msg_id);
                break;
            case MSG_ACK:
                handle_ack(msg.msg_id);
                break;
            case MSG_BYE:
                std::cout << "\n*** Server closed connection ***" << std::endl;
                pthread_mutex_lock(&sock_mutex);
                connected = false;
                close(sock);
                sock = -1;
                pthread_mutex_unlock(&sock_mutex);
                drop_all_pending_as_failed();
                clear_ping_state();
                break;
            case MSG_HISTORY_DATA:
                std::cout << "\n[HISTORY]:\n" << msg.payload << std::endl;
                break;
            default:
                std::cout << "\n*** Unknown message type: " << static_cast<int>(msg.type) << " ***" << std::endl;
                break;
        }

        std::cout << "> ";
        fflush(stdout);
    }

    return nullptr;
}

int connect_to_server() {
    int new_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (new_sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(new_sock);
        return -1;
    }

    if (connect(new_sock, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        perror("connect");
        close(new_sock);
        return -1;
    }

    return new_sock;
}

bool authenticate(int socket_fd, const std::string& nickname) {
    MessageEx auth_msg{};
    strncpy(auth_msg.payload, nickname.c_str(), MAX_NAME - 1);
    auth_msg.payload[MAX_NAME - 1] = '\0';
    auth_msg.length = strlen(auth_msg.payload) + 1;
    auth_msg.type = MSG_AUTH;

    if (!send_message_raw(socket_fd, auth_msg)) return false;

    MessageEx response{};
    if (!recv_message(socket_fd, response)) return false;

    if (response.type == MSG_ERROR) {
        std::cout << "Authentication failed: " << response.payload << std::endl;
        return false;
    }

    if (response.type == MSG_WELCOME) {
        std::cout << response.payload << std::endl;
        return true;
    }

    return false;
}

int parse_ping_count(const char* input) {
    if (strcmp(input, "/ping") == 0) return DEFAULT_PING_COUNT;
    if (strncmp(input, "/ping ", 6) != 0) return -1;

    const char* param = input + 6;
    if (*param == '\0') return -1;
    for (size_t i = 0; param[i] != '\0'; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(param[i]))) return -1;
    }

    int count = std::atoi(param);
    return count > 0 ? count : -1;
}

void run_ping_series(int count) {
    std::vector<double> rtts;
    std::vector<double> jitters;
    double previous_rtt = -1.0;

    for (int i = 0; i < count && keepRunning && connected; ++i) {
        MessageEx ping{};
        strcpy(ping.payload, "ping");
        ping.length = strlen(ping.payload) + 1;
        ping.type = MSG_PING;

        if (!send_reliable_message(ping)) {
            std::cout << "PING " << (i + 1) << " -> send failed" << std::endl;
            continue;
        }

        uint32_t msg_id = ping.msg_id;
        bool pong_received = false;
        double rtt = 0.0;

        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(ACK_TIMEOUT_MS * (MAX_RETRIES + 1));

        std::unique_lock<std::mutex> lock(ping_mutex);
        while (keepRunning && connected) {
            auto pong_it = ping_rtts.find(msg_id);
            if (pong_it != ping_rtts.end()) {
                rtt = pong_it->second;
                ping_rtts.erase(pong_it);
                pong_received = true;
                break;
            }

            if (ping_cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        lock.unlock();

        if (pong_received) {
            rtts.push_back(rtt);
            std::cout << "PING " << (i + 1) << " -> RTT=" << format_double(rtt) << "ms";
            if (previous_rtt >= 0.0) {
                double jitter = std::fabs(rtt - previous_rtt);
                jitters.push_back(jitter);
                std::cout << " | Jitter=" << format_double(jitter) << "ms";
            }
            std::cout << std::endl;
            previous_rtt = rtt;
        } else {
            std::cout << "PING " << (i + 1) << " -> timeout" << std::endl;
            std::lock_guard<std::mutex> ping_state_lock(ping_mutex);
            ping_send_times.erase(msg_id);
            ping_rtts.erase(msg_id);
        }
    }

    update_last_diag(count, rtts, jitters);
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    pthread_t recv_thread;
    pthread_t retry_thr;

    std::cout << "Enter your nickname: ";
    std::getline(std::cin, g_nickname);
    while (g_nickname.empty()) {
        std::cout << "Nickname cannot be empty. Enter your nickname: ";
        std::getline(std::cin, g_nickname);
    }

    pthread_create(&retry_thr, NULL, retry_thread, NULL);

    while (keepRunning) {
        std::cout << "Connecting to " << SERVER_IP << ":" << SERVER_PORT << "..." << std::endl;

        pthread_mutex_lock(&sock_mutex);
        sock = connect_to_server();
        pthread_mutex_unlock(&sock_mutex);

        if (sock < 0) {
            std::cout << "Connection failed. Retrying in " << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
            continue;
        }

        if (!authenticate(sock, g_nickname)) {
            std::cout << "Authentication failed" << std::endl;
            pthread_mutex_lock(&sock_mutex);
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            sleep(RECONNECT_DELAY);
            keepRunning = false;
            break;
        }

        connected = true;
        std::cout << "Connected to server. Type messages:" << std::endl;

        if (pthread_create(&recv_thread, NULL, receive_thread, NULL) != 0) {
            perror("pthread_create");
            break;
        }

        char input[MAX_PAYLOAD];
        std::cout << "> " << std::flush;

        while (keepRunning && connected) {
            if (!read_input_with_timeout(input, MAX_PAYLOAD, INPUT_TIMEOUT)) continue;
            if (strlen(input) == 0) continue;

            MessageEx msg{};

            if (strcmp(input, "/quit") == 0) {
                strcpy(msg.payload, "bye");
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_BYE;
                send_over_socket(msg);
                connected = false;
                keepRunning = false;
                break;
            } else if (strncmp(input, "/ping", 5) == 0) {
                int count = parse_ping_count(input);
                if (count <= 0) {
                    std::cout << "Usage: /ping or /ping <positive_number>" << std::endl;
                } else {
                    run_ping_series(count);
                }
            } else if (strcmp(input, "/netdiag") == 0) {
                print_netdiag();
            } else if (strncmp(input, "/w ", 3) == 0) {
                char buffer[MAX_PAYLOAD];
                strncpy(buffer, input, sizeof(buffer) - 1);
                buffer[sizeof(buffer) - 1] = '\0';
                char* target = strtok(buffer + 3, " ");
                char* message = strtok(NULL, "");
                if (target && message && strlen(message) > 0) {
                    snprintf(msg.payload, MAX_PAYLOAD, "%s", message);
                    msg.length = strlen(msg.payload) + 1;
                    msg.type = MSG_PRIVATE;
                    snprintf(msg.receiver, MAX_NAME, "%s", target);
                    send_reliable_message(msg);
                } else {
                    std::cout << "Usage: /w <nickname> <message>" << std::endl;
                }
            } else if (strcmp(input, "/help") == 0) {
                std::cout << "  /help                              - Show this help message\n";
                std::cout << "  /list                              - Show online users list\n";
                std::cout << "  /history                           - Show all last messages\n";
                std::cout << "  /history N                         - Show last N messages\n";
                std::cout << "  /netdiag                           - Show last ping statistics\n";
                std::cout << "  /ping                              - Send 10 ping requests\n";
                std::cout << "  /ping N                            - Send N ping requests\n";
                std::cout << "  /quit                              - Disconnect from server\n";
                std::cout << "  /w <nick> <message>                - Send private message\n";
            } else if (strcmp(input, "/list") == 0) {
                strcpy(msg.payload, "list");
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_LIST;
                send_over_socket(msg);
            } else if (strncmp(input, "/history", 8) == 0) {
                if (strlen(input) == 8) {
                    msg.payload[0] = '\0';
                    msg.length = 1;
                    msg.type = MSG_HISTORY;
                    send_over_socket(msg);
                } else if (input[8] == ' ') {
                    const char* param = input + 9;
                    bool valid = strlen(param) > 0;
                    for (size_t i = 0; param[i] != '\0'; ++i) {
                        if (!std::isdigit(static_cast<unsigned char>(param[i]))) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid || std::atoi(param) <= 0) {
                        std::cout << "Error: Invalid parameter. Use /history or /history <positive_number>" << std::endl;
                    } else {
                        snprintf(msg.payload, MAX_PAYLOAD, "%s", param);
                        msg.length = strlen(msg.payload) + 1;
                        msg.type = MSG_HISTORY;
                        send_over_socket(msg);
                    }
                } else {
                    std::cout << "Error: Use /history or /history <positive_number>" << std::endl;
                }
            } else {
                strncpy(msg.payload, input, MAX_PAYLOAD - 1);
                msg.payload[MAX_PAYLOAD - 1] = '\0';
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_TEXT;
                send_reliable_message(msg);
            }

            std::cout << "> " << std::flush;
        }

        pthread_join(recv_thread, NULL);

        if (keepRunning && !connected) {
            std::cout << "Attempting to reconnect in " << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
        }
    }

    keepRunning = false;
    pending_cv.notify_all();
    ping_cv.notify_all();
    pthread_join(retry_thr, NULL);

    pthread_mutex_lock(&sock_mutex);
    if (sock >= 0) close(sock);
    pthread_mutex_unlock(&sock_mutex);

    std::cout << "Client shutdown" << std::endl;
    return 0;
}
