#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>
#include <errno.h>
#include <stack>
#include <pthread.h>
#include <iostream>
#include <vector>

using namespace std;

// Global timing variables
struct timeval transfer_start_time;
struct timeval transfer_end_time;

#define PACKET_SIZE 1024
#define DATA_SIZE (PACKET_SIZE - sizeof(int) * 4)
#define TIMEOUT_MS 100  // 100ms timeout for ACKs
#define MAX_RETRIES 5
#define INITIAL_WINDOW_SIZE 10
#define MAX_WINDOW_SIZE 100

#define PACKET_DATA 1
#define PACKET_ACK 2

static std::vector<struct packet> packet_buffer;
static int global_total_packets = 0;
static std::stack<struct packet> packet_stack;
static volatile bool sender_finished = false;

pthread_mutex_t stack_mutex = PTHREAD_MUTEX_INITIALIZER;

struct packet {
    int sequence_number;
    int total_packets;
    int data_size;
    int packet_type;
    char data[DATA_SIZE];
};

struct nack_packet {
    int packet_number;
};

struct sender_args {
    int sockfd;
    struct sockaddr_in serv_addr;
};

void error(const char *msg) {
    perror(msg);
    exit(1);
}

// Function to create socket and setup server address
int setup_connection(const char* hostname, const char* port, struct sockaddr_in* serv_addr) {
    // Create socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        error("ERROR opening socket");
    }

    // Setup server address
    struct hostent *server = gethostbyname(hostname);
    if (server == NULL) {
        error("ERROR, no such host");
    }

    memset(serv_addr, 0, sizeof(*serv_addr));
    serv_addr->sin_family = AF_INET;
    memcpy(&serv_addr->sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr->sin_port = htons(atoi(port));

    return sockfd;
}

// break the file into packets and load them into the packet_buffer vector
int preload_data(const char *filename) {
    cout << "=== PRELOADING PHASE ===" << endl;
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        error("ERROR opening file");
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    global_total_packets = (file_size + DATA_SIZE - 1) / DATA_SIZE;
    cout << "File size: " << file_size << " bytes" << endl;
    cout << "Total packets: " << global_total_packets << endl;

    // Allocate packet buffer using vector
    packet_buffer.resize(global_total_packets);
    
    cout << "Loading packets into buffer..." << endl;
    
    // Load packets into packet_buffer vector
    for (int i = 0; i < global_total_packets; i++) {
        fseek(file, i * DATA_SIZE, SEEK_SET);
        int bytes_read = fread(packet_buffer[i].data, 1, DATA_SIZE, file);

        packet_buffer[i].sequence_number = i;
        packet_buffer[i].total_packets = global_total_packets;
        packet_buffer[i].data_size = bytes_read;
        packet_buffer[i].packet_type = PACKET_DATA;

        if (i % 10000 == 0) {
            cout << "Preloaded packet " << i << "/" << global_total_packets << endl;
        }
    }
    fclose(file);
    cout << "Preloading complete!" << endl;
    cout << "=== END PRELOADING PHASE ===" << endl << endl;

    return global_total_packets;
}

void load_packets_into_stack() {
    for (int i = global_total_packets - 1; i >= 0; i--) {
        packet_stack.push(packet_buffer[i]);
    }
}

// The sender thread function
void* sender_thread_func(void* arg) {
    sender_args* args = (sender_args*)arg;
    cout << "----- Sender thread started -----" << endl;
    
    bool first_packet = true;
    bool initial_send_complete = false;
    
    while (!sender_finished) {
        pthread_mutex_lock(&stack_mutex);
        // Check if stack is empty
        if (packet_stack.empty()) {
            if (!initial_send_complete) {
                initial_send_complete = true;
                cout << "----- Initial packet transmission complete, waiting for retransmissions -----" << endl;
            }
            pthread_mutex_unlock(&stack_mutex);
            usleep(1000); // Sleep 1ms and check again
            continue;
        }

        struct packet p = packet_stack.top();
        packet_stack.pop();
        pthread_mutex_unlock(&stack_mutex);

        // Record start time when sending the very first packet
        if (first_packet) {
            gettimeofday(&transfer_start_time, NULL);
            cout << "----- Transfer started - timing begins -----" << endl;
            first_packet = false;
        }

        sendto(args->sockfd, &p, sizeof(p), 0,
               (struct sockaddr*)&args->serv_addr, sizeof(args->serv_addr));

        if (initial_send_complete) {
            cout << "Retransmitting packet " << p.sequence_number << endl;
        }

        // add small delay to prevent buffer overflow
        usleep(40);
    }

    cout << "----- Sender thread finished (stopped by completion signal) -----" << endl;
    return NULL;
}

// accept nacks from the server
// terminates when sender thread finishes OR when server sends completion signal
void *nack_thread_func(void *arg) {
    cout << "----- Nack thread started -----" << endl;
    sender_args* args = (sender_args*)arg;
    struct nack_packet nack;
    socklen_t addrlen = sizeof(args->serv_addr);
    
    // Set socket timeout for non-blocking receive
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT_MS * 1000; // Convert ms to microseconds
    
    if (setsockopt(args->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
    }

    while (!sender_finished) {
        int n = recvfrom(args->sockfd, &nack, sizeof(nack), 0, 
                        (struct sockaddr*)&args->serv_addr, &addrlen);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - continue waiting
                continue;
            } else {
                // Real error occurred
                cout << "----- Error receiving nack: " << strerror(errno) << " -----" << endl;
                continue;
            }
        }
        
        // Check for completion signal from server
        if (nack.packet_number == -1) {
            cout << "Received completion signal from server (NACK -1)" << endl;
            cout << "All packets successfully received by server!" << endl;
            
            // Record end time immediately when server confirms completion
            gettimeofday(&transfer_end_time, NULL);
            
            // Signal sender thread to stop
            sender_finished = true;
            
            // Clear any remaining packets from stack since transfer is complete
            pthread_mutex_lock(&stack_mutex);
            while (!packet_stack.empty()) {
                packet_stack.pop();
            }
            pthread_mutex_unlock(&stack_mutex);
            
            cout << "----- Transfer completed successfully -----" << endl;
            break;
        }
        
        // Handle regular NACK for retransmission
        if (nack.packet_number >= 0 && nack.packet_number < global_total_packets) {
            cout << "Received NACK for packet " << nack.packet_number << ", retransmitting..." << endl;
            
            pthread_mutex_lock(&stack_mutex);
            packet_stack.push(packet_buffer[nack.packet_number]);
            pthread_mutex_unlock(&stack_mutex);
        } else if (nack.packet_number != -1) {
            cout << "----- NACK out of range: " << nack.packet_number << " -----" << endl;
        }
    }
    
    // If we exit the loop because sender_finished naturally (not because of completion signal)
    // we still need to record end time if it wasn't already recorded
    if (transfer_end_time.tv_sec == 0) {
        gettimeofday(&transfer_end_time, NULL);
    }
    
    cout << "----- NACK thread finished -----" << endl;
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        cout << "Usage: " << argv[0] << " hostname port local_file remote_file" << endl;
        cout << "Example: " << argv[0] << " 192.168.1.100 8080 data.bin received_data.bin" << endl;
        exit(1);
    }

    const char* hostname = argv[1];
    const char* port = argv[2]; 
    const char* local_file = argv[3];
    const char* remote_file = argv[4];

    cout << "Transferring '" << local_file << "' to " << hostname << ":" << port << " as '" << remote_file << "'" << endl;

    // PRELOAD PHASE
    if (preload_data(local_file) == 0) {
        error("ERROR preloading data");
    }

    load_packets_into_stack();

    // Connect to server
    struct sockaddr_in serv_addr;
    int sockfd = setup_connection(hostname, port, &serv_addr);

    // Start sender thread
    sender_args args = {sockfd, serv_addr};
    pthread_t sender_thread;
    pthread_create(&sender_thread, NULL, sender_thread_func, &args);

    // Start nack thread
    pthread_t nack_thread;
    pthread_create(&nack_thread, NULL, nack_thread_func, &args);

    // Wait for sender thread to finish
    pthread_join(sender_thread, NULL);

    // Wait for nack thread to finish
    pthread_join(nack_thread, NULL);

    // Calculate and display transfer performance
    double transfer_time = (transfer_end_time.tv_sec - transfer_start_time.tv_sec) + 
                          (transfer_end_time.tv_usec - transfer_start_time.tv_usec) / 1000000.0;
    
    long file_size_bytes = global_total_packets * DATA_SIZE;
    double file_size_mb = file_size_bytes / (1024.0 * 1024.0);
    double throughput_mbps = (file_size_mb * 8) / transfer_time;
    
    cout << "\n===== TRANSFER PERFORMANCE =====" << endl;
    cout << "File size: " << file_size_mb << " MB" << endl;
    cout << "Transfer time: " << transfer_time << " seconds" << endl;
    cout << "Throughput: " << throughput_mbps << " Mbps" << endl;
    cout << "=================================" << endl;

    close(sockfd);
    return 0;
}