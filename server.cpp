#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <openssl/md5.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// ---------------- Protocol ----------------
#define PACKET_SIZE 1024
#define DATA_SIZE (PACKET_SIZE - sizeof(int) * 4)

#define PACKET_DATA 1
#define PACKET_ACK  2
#define PACKET_NACK 3

#define RTT 200 // ms

struct packet {
    int sequence_number;
    int total_packets;
    int data_size;
    int packet_type;
    char data[DATA_SIZE];
};

struct nack_packet {
    int packet_num;
};

struct critical_error {
    std::string error;
};

std::vector<critical_error> critical_errors;

// ---------------- Global Variables ----------------
static int total_packets_expected = -1;
static bool transfer_complete = false;
static std::mutex completion_mutex;

// ---------------- Queue ----------------
struct Node {
    int packet_num;
    timeval t;
    bool nack_sent = false; 
};

class PacketQueue {
    std::list<Node> dq;
    std::unordered_map<int, std::list<Node>::iterator> mp;
    mutable std::mutex mtx;

public:
    void push_back(int packet_num, timeval t) {
        std::lock_guard<std::mutex> lock(mtx);
        if (mp.count(packet_num)) return;
        dq.push_back({packet_num, t});
        mp[packet_num] = std::prev(dq.end());
    }

    void push_front(int packet_num, timeval t) {
        std::lock_guard<std::mutex> lock(mtx);
        if (mp.count(packet_num)) return;
        dq.push_front({packet_num, t});
        mp[packet_num] = dq.begin();
    }
    
    bool remove(int packet_num, bool &was_nacked) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = mp.find(packet_num);
        if (it != mp.end()) {
            was_nacked = it->second->nack_sent;
            dq.erase(it->second);
            mp.erase(it);
            return true;
        }
        return false;
    }

    void print_contents() {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[Queue] ";
        for (const auto& n : dq) {
            std::cout << n.packet_num << " ";
        }
        std::cout << std::endl;
    }

    Node front() {
        std::lock_guard<std::mutex> lock(mtx);
        if (dq.empty()) return {-1, {0,0}};
        return dq.front();
    }

    int requeue_front_with_now() {
        std::lock_guard<std::mutex> lock(mtx);
        if (dq.empty()) return -1;
        Node n = dq.front();
        dq.pop_front();
        mp.erase(n.packet_num);
        gettimeofday(&n.t, NULL);
        n.nack_sent = true;   // <--- mark as NACKed
        dq.push_back(n);
        mp[n.packet_num] = std::prev(dq.end());
        return n.packet_num;
    }

    void enqueue_missing_between(int last, int current) {
        std::lock_guard<std::mutex> lock(mtx);
        for (int pkt = last + 1; pkt < current; ++pkt) {
            if (!mp.count(pkt)) {
                dq.push_front({pkt, {0,0}, false});
                mp[pkt] = dq.begin();
            }
        }
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return dq.empty();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return dq.size();
    }
};

PacketQueue lost_packets_queue;
std::unordered_map<int, std::vector<char>> reassembly_map;
std::unordered_map<int, int> packet_sizes;  // Store actual data sizes
std::mutex reassembly_mutex;

// ---------------- Helpers ----------------
bool packet_added(int packet_num) {
    std::lock_guard<std::mutex> lock(reassembly_mutex);
    return reassembly_map.find(packet_num) != reassembly_map.end();
}

void add_packet(const packet& pkt) {
    std::lock_guard<std::mutex> lock(reassembly_mutex);
    if (reassembly_map.find(pkt.sequence_number) != reassembly_map.end()) return;
    reassembly_map[pkt.sequence_number] =
        std::vector<char>(pkt.data, pkt.data + pkt.data_size);
    packet_sizes[pkt.sequence_number] = pkt.data_size;  // Store actual size
    // std::cout << "[Receiver] Stored packet " << pkt.sequence_number << " with " << pkt.data_size << " bytes\n";
}

bool check_transfer_complete() {
    std::lock_guard<std::mutex> comp_lock(completion_mutex);
    std::lock_guard<std::mutex> reasm_lock(reassembly_mutex);
    
    if (total_packets_expected == -1) return false;
    if (transfer_complete) return true;
    
    // Check if we have all packets and no missing packets in queue
    if ((int)reassembly_map.size() == total_packets_expected && lost_packets_queue.empty()) {
        transfer_complete = true;
        std::cout << "[Server] All " << total_packets_expected << " packets received! Transfer complete!" << std::endl;
        return true;
    }
    return false;
}

bool expired(const timeval& t, long threshold_ms) {
    if (t.tv_sec == 0 && t.tv_usec == 0) return true;
    timeval now;
    gettimeofday(&now, NULL);
    long diff_ms = (now.tv_sec - t.tv_sec) * 1000L +
                   (now.tv_usec - t.tv_usec) / 1000L;
    return diff_ms > threshold_ms;
}

// ---------------- Globals ----------------
int sockfd;
sockaddr_in cli_addr;
socklen_t cli_len = sizeof(cli_addr);

// ---------------- Threads ----------------
void* receiver_thread(void* arg) {
    packet pkt{};
    int last_successful = -1;

    while (1) {
        int n = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                         (struct sockaddr*)&cli_addr, &cli_len);
        if (n < 0) {
            perror("[Receiver] recvfrom");
            continue;
        }
        if (pkt.packet_type != PACKET_DATA) continue;

        // Set total packets expected from first packet
        if (total_packets_expected == -1) {
            total_packets_expected = pkt.total_packets;
            std::cout << "[Receiver] Expecting " << total_packets_expected << " total packets" << std::endl;
        }

        // Handle out-of-order packets
        if (pkt.sequence_number > last_successful + 1) {
            lost_packets_queue.enqueue_missing_between(last_successful, pkt.sequence_number);
        }

        // Remove from lost packets queue if it was there
        bool was_nacked = false;
        if (lost_packets_queue.remove(pkt.sequence_number, was_nacked)) {
            if (was_nacked) {
                // std::cout << "[Receiver] Retrieved Retransmitted Packet "
                //         << pkt.sequence_number << "\n";
            } else {
                // std::cout << "[Receiver] Retrieved Late Packet "
                //         << pkt.sequence_number << "\n";
            }
        }

        if (pkt.sequence_number % 10000 == 0) {
            lost_packets_queue.print_contents();
        }

        // Store the packet data
        add_packet(pkt);

        // Update last successful packet
        if (pkt.sequence_number > last_successful) {
            last_successful = pkt.sequence_number;
        }

        // Check if transfer is complete
        check_transfer_complete();
    }
    
    return nullptr;
}

void* sender_thread(void* arg) {
    nack_packet np{};
    bool completion_sent = false;
    
    while (1) {
        // Check if transfer is complete and send completion signal
        if (check_transfer_complete() && !completion_sent) {
            np.packet_num = -1;  // Completion signal
            for(int i = 0; i < 5; i++){
                int s = sendto(sockfd, &np, sizeof(np), 0,
                            (struct sockaddr*)&cli_addr, cli_len);
                if (s < 0) {
                    perror("[Sender] sendto completion signal");
                } else {
                    std::cout << "[Sender] Sent completion signal (NACK -1) to client" << std::endl;
                    completion_sent = true;
                }
                usleep(10);
            }

            
            break;  // Exit sender thread after sending completion signal
        }

        // Handle regular NACKs for missing packets
        Node n = lost_packets_queue.front();
        if (n.packet_num != -1 && expired(n.t, 2 * RTT)) {
            int pktnum = lost_packets_queue.requeue_front_with_now();
            np.packet_num = pktnum;
            int s = sendto(sockfd, &np, sizeof(np), 0,
                           (struct sockaddr*)&cli_addr, cli_len);
            if (s < 0) {
                perror("[Sender] sendto NACK");
                critical_errors.push_back({"Failed to send NACK"});
            } else {
                // std::cout << "[Sender] Sent NACK for " << pktnum << "\n";
            }
        }
    }
    
    std::cout << "[Sender] Sender thread finished" << std::endl;
    return nullptr;
}

// ---------------- Main ----------------
void error(const char* msg) {
    perror(msg);
    exit(1);
}

std::string compute_md5(const std::string& filename) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_CTX mdContext;
    MD5_Init(&mdContext);

    std::ifstream file(filename, std::ifstream::binary);

    const size_t bufSize = 16384; // 16KB buffer
    std::vector<char> buffer(bufSize);

    while (file.good()) {
        file.read(buffer.data(), bufSize);
        MD5_Update(&mdContext, buffer.data(), file.gcount());
    }

    MD5_Final(digest, &mdContext);

    // Convert digest to hex string
    std::ostringstream md5string;
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        md5string << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return md5string.str();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) error("socket");

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        error("bind");
    }

    std::cout << "Server listening on port " << port << "\n";

    pthread_t recv_tid, send_tid;
    pthread_create(&recv_tid, NULL, receiver_thread, NULL);
    pthread_create(&send_tid, NULL, sender_thread, NULL);

    // Wait for sender thread to complete (when transfer finishes)
    pthread_join(send_tid, NULL);
    
    std::cout << "File transfer completed successfully!" << std::endl;
    
    // Cancel receiver thread since transfer is done
    pthread_cancel(recv_tid);

    // Write the file using actual data sizes
    std::ofstream out("received_data.bin", std::ios::binary);
    long total_bytes_written = 0;
    for (int seq = 0; seq < total_packets_expected; ++seq) {
        const auto& buf = reassembly_map[seq];
        int actual_size = packet_sizes[seq];           // Get actual size from stored sizes
        out.write(buf.data(), actual_size);            // Write only actual data
        total_bytes_written += actual_size;
    }
    out.close();

    std::cout << "Total bytes written: " << total_bytes_written << std::endl;
    
    std::string original_md5 = compute_md5("data.bin");          
    std::string received_md5 = compute_md5("received_data.bin");

    std::cout << "Original MD5:  " << original_md5 << std::endl;
    std::cout << "Received MD5:  " << received_md5 << std::endl;

    if (original_md5 == received_md5) {
        std::cout << "✅ Files are identical (transfer successful)" << std::endl;
    } else {
        std::cout << "❌ Files differ (transfer corrupted)" << std::endl;
    }

    close(sockfd);
    return 0;
}