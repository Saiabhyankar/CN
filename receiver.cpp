#include "udp_file_transfer.h"
#include <signal.h>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <libgen.h>
#include <random>

class FileReceiver {
private:
    int sockfd;
    struct sockaddr_in server_addr, sender_addr;
    std::string output_filename;
    std::string output_dir;
    int record_size;
    int blast_size;
    long file_size;
    long total_records;
    std::map<int, std::vector<char>> received_records;
    int packets_received;

    double loss_percent;  // % of simulated packet loss
    std::mt19937 rng;     // random number generator
    std::uniform_real_distribution<double> dist;

public:
    FileReceiver(int port, const std::string &out_dir, double loss_pct)
        : loss_percent(loss_pct), rng(std::random_device{}()), dist(0.0, 100.0) {
        packets_received = 0;
        output_dir = out_dir;

        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Bind failed");
            exit(1);
        }

        std::cout << "Receiver listening on port " << port
                  << " (output dir: " << out_dir << ", loss=" << loss_percent << "%)" << std::endl;
    }

    ~FileReceiver() {
        close(sockfd);
    }

    bool phase1_connection_setup() {
        std::cout << "Waiting for connection..." << std::endl;

        FileHeader hdr;
        socklen_t len = sizeof(sender_addr);

        int n = recvfrom(sockfd, &hdr, sizeof(hdr), 0,
                         (struct sockaddr*)&sender_addr, &len);

        if (n > 0 && hdr.type == FILE_HDR) {
            file_size = hdr.file_size;
            record_size = hdr.record_size;
            blast_size = hdr.blast_size;
            std::string recv_name = hdr.filename;

            // Create output directory if needed
            if (!ensure_directory_exists(output_dir)) {
                std::cerr << "Error: cannot access output directory " << output_dir << std::endl;
                return false;
            }

            // Set output filename
            char recv_name_copy[256];
            strncpy(recv_name_copy, recv_name.c_str(), 255);
            recv_name_copy[255] = '\0';
            std::string filename_only = std::string(basename(recv_name_copy));

            output_filename = output_dir;
            if (output_filename.back() != '/') output_filename += '/';
            output_filename += filename_only;
            output_filename += ".received";

            total_records = (file_size + record_size - 1) / record_size;

            std::cout << "Received FILE_HDR:" << std::endl;
            std::cout << "  File: " << filename_only << std::endl;
            std::cout << "  Size: " << file_size << " bytes" << std::endl;
            std::cout << "  Record size: " << record_size << " bytes" << std::endl;
            std::cout << "  Blast size: " << blast_size << " records" << std::endl;
            std::cout << "  Writing to: " << output_filename << std::endl;

            // Send acknowledgment
            FileHeader ack;
            ack.type = FILE_HDR_ACK;
            sendto(sockfd, &ack, sizeof(ack), 0,
                   (struct sockaddr*)&sender_addr, len);

            std::cout << "Sent FILE_HDR_ACK" << std::endl;
            return true;
        }

        return false;
    }

    void phase2_data_transfer() {
        std::cout << "\nPhase 2: Receiving data..." << std::endl;

        std::vector<char> buffer(MAX_PACKET_SIZE);
        socklen_t len = sizeof(sender_addr);

        while (true) {
            int n = recvfrom(sockfd, buffer.data(), MAX_PACKET_SIZE, 0,
                             (struct sockaddr*)&sender_addr, &len);

            if (n <= 0) continue;

            // Simulate packet loss
            double random_value = dist(rng);
            if (random_value < loss_percent) {
                std::cout << "[Simulated LOSS] Dropping packet (random=" << random_value << "%)" << std::endl;
                continue; // skip this packet
            }

            PacketType type = *(PacketType*)buffer.data();

            if (type == DATA_PACKET) {
                packets_received++;
                process_data_packet(buffer.data(), n);
            }
            else if (type == IS_BLAST_OVER) {
                BlastQuery* query = (BlastQuery*)buffer.data();
                std::cout << "Received IS_BLAST_OVER(" << query->m_start
                          << ", " << query->m_end << ")" << std::endl;
                send_missing_records(query->m_start, query->m_end);
            }
            else if (type == DISCONNECT) {
                std::cout << "Received DISCONNECT" << std::endl;
                break;
            }
        }
    }

    void process_data_packet(char* buffer, int size) {
        DataPacketHeader* hdr = (DataPacketHeader*)buffer;
        char* data = buffer + sizeof(DataPacketHeader);

        for (int seg = 0; seg < hdr->num_segments; seg++) {
            int start = hdr->segments[seg].start;
            int end = hdr->segments[seg].end;

            for (int rec = start; rec <= end; rec++) {
                int offset = (rec - start) * record_size;
                std::vector<char> record_data(record_size);

                int bytes_to_copy = record_size;
                if (rec == total_records - 1) {
                    bytes_to_copy = file_size - (rec * record_size);
                }

                memcpy(record_data.data(), data + offset, bytes_to_copy);
                received_records[rec] = record_data;
            }
        }
    }

    void send_missing_records(int blast_start, int blast_end) {
        MissingRecordsPacket miss;
        miss.type = REC_MISS;
        miss.num_ranges = 0;

        int range_start = -1;
        for (int rec = blast_start; rec <= blast_end; rec++) {
            if (received_records.find(rec) == received_records.end()) {
                if (range_start == -1) range_start = rec;
            } else if (range_start != -1) {
                miss.ranges[miss.num_ranges++] = {range_start, rec - 1};
                range_start = -1;
            }
        }
        if (range_start != -1) {
            miss.ranges[miss.num_ranges++] = {range_start, blast_end};
        }

        if (miss.num_ranges == 0) {
            std::cout << "✅ All records received for this blast." << std::endl;
        } else {
            std::cout << "⚠ Missing " << miss.num_ranges << " range(s): ";
            for (int i = 0; i < miss.num_ranges; i++)
                std::cout << "[" << miss.ranges[i].start << "-" << miss.ranges[i].end << "] ";
            std::cout << std::endl;
        }

        socklen_t len = sizeof(sender_addr);
        sendto(sockfd, &miss, sizeof(miss), 0, (struct sockaddr*)&sender_addr, len);
    }

    void phase3_write_file() {
        std::cout << "\nPhase 3: Writing file to disk..." << std::endl;

        std::ofstream outfile(output_filename, std::ios::binary);
        if (!outfile) {
            std::cerr << "Error: cannot open " << output_filename << std::endl;
            return;
        }

        for (int rec = 0; rec < total_records; rec++) {
            if (received_records.find(rec) != received_records.end()) {
                int bytes_to_write = (rec == total_records - 1)
                    ? file_size - (rec * record_size)
                    : record_size;
                outfile.write(received_records[rec].data(), bytes_to_write);
            } else {
                std::cerr << "❌ Record " << rec << " missing!" << std::endl;
            }
        }

        outfile.close();
        std::cout << "✅ File written successfully: " << output_filename << std::endl;
        std::cout << "📦 Total packets received: " << packets_received << std::endl;
    }

    void receive_file() {
        if (!phase1_connection_setup()) return;
        phase2_data_transfer();
        phase3_write_file();
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <port> <output_dir> [loss_percent]" << std::endl;
        return 1;
    }

    int port = atoi(argv[1]);
    std::string outdir = argv[2];
    double loss_pct = (argc >= 4) ? atof(argv[3]) : 0.0;  // optional loss percentage

    FileReceiver receiver(port, outdir, loss_pct);
    receiver.receive_file();

    return 0;
}

