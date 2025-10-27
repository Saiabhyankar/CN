
#include "udp_file_transfer.h"
#include <ctime>
#include <algorithm>

class FileSender {
private:
    int sockfd;
    struct sockaddr_in receiver_addr;
    std::string filename;
    int record_size;
    int blast_size;
    long file_size;
    int packets_sent;
    int packets_lost;
    clock_t start_time;

public:
    FileSender(const char* ip, int port, const char* file, int rec_size = 512, int blast = 1000) {
        filename = file;
        record_size = rec_size;
        blast_size = blast;
        packets_sent = 0;
        packets_lost = 0;

        // Create UDP socket
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }

        memset(&receiver_addr, 0, sizeof(receiver_addr));
        receiver_addr.sin_family = AF_INET;
        receiver_addr.sin_port = htons(port);
        receiver_addr.sin_addr.s_addr = inet_addr(ip);

        file_size = get_file_size(file);
        if (file_size < 0) {
            std::cerr << "Error: Cannot open file " << file << std::endl;
            exit(1);
        }
    }

    ~FileSender() {
        close(sockfd);
    }

    bool phase1_connection_setup() {
        std::cout << "Phase 1: Connection Setup" << std::endl;

        FileHeader hdr;
        hdr.type = FILE_HDR;
        hdr.file_size = file_size;
        hdr.record_size = record_size;
        hdr.blast_size = blast_size;
        strncpy(hdr.filename, filename.c_str(), 255);
        hdr.filename[255] = '\0';

        set_socket_timeout(sockfd, TIMEOUT_SEC);

        for (int retry = 0; retry < MAX_RETRIES; retry++) {
            sendto(sockfd, &hdr, sizeof(hdr), 0,
                   (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
            packets_sent++;
            std::cout << "Sent FILE_HDR (attempt " << retry + 1 << ")" << std::endl;

            FileHeader ack;
            socklen_t len = sizeof(receiver_addr);
            int n = recvfrom(sockfd, &ack, sizeof(ack), 0,
                           (struct sockaddr*)&receiver_addr, &len);

            if (n > 0 && ack.type == FILE_HDR_ACK) {
                std::cout << "Received FILE_HDR_ACK - Connection established" << std::endl;
                return true;
            }
        }

        std::cerr << "Connection setup failed after " << MAX_RETRIES << " attempts" << std::endl;
        return false;
    }

    void phase2_data_transfer() {
        std::cout << "\nPhase 2: Data Transfer" << std::endl;
        start_time = clock();

        std::ifstream file(filename, std::ios::binary);
        if (!file) {
            std::cerr << "Error opening file" << std::endl;
            return;
        }

        long total_records = (file_size + record_size - 1) / record_size;
        int current_record = 0;

        while (current_record < total_records) {
            int blast_start = current_record;
            int blast_end = std::min(current_record + blast_size - 1, (int)total_records - 1);

            std::cout << "Sending blast: records " << blast_start << " to " << blast_end << std::endl;

            // Send all records in blast
            send_blast_records(file, blast_start, blast_end);

            // Handle retransmissions
            handle_blast_retransmissions(file, blast_start, blast_end);

            current_record = blast_end + 1;
        }

        file.close();

        double elapsed = (double)(clock() - start_time) / CLOCKS_PER_SEC;
        double throughput = (file_size / (1024.0 * 1024.0)) / (elapsed > 0 ? elapsed : 1.0);

        std::cout << "\n=== Transfer Statistics ===" << std::endl;
        std::cout << "File size: " << file_size << " bytes" << std::endl;
        std::cout << "Packets sent: " << packets_sent << std::endl;
        std::cout << "Packets lost (reported by receiver): " << packets_lost << std::endl;
        std::cout << "Time elapsed: " << elapsed << " seconds" << std::endl;
        std::cout << "Throughput: " << throughput << " MB/s" << std::endl;
    }

    void send_blast_records(std::ifstream& file, int start, int end) {
        long total_records = (file_size + record_size - 1) / record_size;
        std::vector<char> buffer(record_size * MAX_RECORDS_PER_PACKET);

        for (int rec = start; rec <= end; ) {
            DataPacketHeader pkt_hdr;
            pkt_hdr.type = DATA_PACKET;
            pkt_hdr.num_segments = 1;

            int records_in_packet = std::min(MAX_RECORDS_PER_PACKET, end - rec + 1);
            pkt_hdr.segments[0].start = rec;
            pkt_hdr.segments[0].end = rec + records_in_packet - 1;

            // Read records from file
            file.seekg(rec * (long)record_size);
            int bytes_to_read = records_in_packet * record_size;
            if (rec + records_in_packet > total_records) {
                bytes_to_read = (int)(file_size - (rec * (long)record_size));
            }
            file.read(buffer.data(), bytes_to_read);
            int actually_read = file.gcount();

            // Create packet
            std::vector<char> packet(sizeof(pkt_hdr) + actually_read);
            memcpy(packet.data(), &pkt_hdr, sizeof(pkt_hdr));
            if (actually_read > 0) memcpy(packet.data() + sizeof(pkt_hdr), buffer.data(), actually_read);

            sendto(sockfd, packet.data(), (socklen_t)packet.size(), 0,
                   (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
            packets_sent++;

            rec += records_in_packet;
        }
    }

    void handle_blast_retransmissions(std::ifstream& file, int start, int end) {
        BlastQuery query;
        query.type = IS_BLAST_OVER;
        query.m_start = start;
        query.m_end = end;

        set_socket_timeout(sockfd, TIMEOUT_SEC);

        while (true) {
            sendto(sockfd, &query, sizeof(query), 0,
                (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
            packets_sent++;

            MissingRecordsPacket miss;
            socklen_t len = sizeof(receiver_addr);
            int n = recvfrom(sockfd, &miss, sizeof(miss), 0,
                            (struct sockaddr*)&receiver_addr, &len);

            if (n > 0 && miss.type == REC_MISS) {
                if (miss.num_ranges == 0) {
                    std::cout << "Blast complete - all records received" << std::endl;
                    break;
                }

                // Log exactly which ranges are missing
                std::cout << "Retransmitting missing ranges: ";
                for (int i = 0; i < miss.num_ranges; i++) {
                    std::cout << "[" << miss.ranges[i].start << "-" << miss.ranges[i].end << "] ";
                }
                std::cout << std::endl;

                packets_lost += miss.num_ranges;

                // Retransmit missing records
                for (int i = 0; i < miss.num_ranges; i++) {
                    send_blast_records(file, miss.ranges[i].start, miss.ranges[i].end);
                }
            }
        }
    }


    void phase3_disconnect() {
        std::cout << "\nPhase 3: Disconnect" << std::endl;

        DisconnectPacket disc;
        disc.type = DISCONNECT;

        sendto(sockfd, &disc, sizeof(disc), 0,
               (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
        packets_sent++;

        std::cout << "Disconnect sent" << std::endl;
    }

    void transfer_file() {
        if (!phase1_connection_setup()) {
            return;
        }
        phase2_data_transfer();
        phase3_disconnect();
    }
};

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cout << "Usage: " << argv[0] << " <receiver_ip> <port> <filename> [record_size] [blast_size]" << std::endl;
        std::cout << "  record_size: 256, 512, or 1024 (default: 512)" << std::endl;
        std::cout << "  blast_size: number of records per blast (default: 1000)" << std::endl;
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);
    const char* filename = argv[3];
    int record_size = (argc > 4) ? atoi(argv[4]) : 512;
    int blast_size = (argc > 5) ? atoi(argv[5]) : 1000;

    FileSender sender(ip, port, filename, record_size, blast_size);
    sender.transfer_file();

    return 0;
}
