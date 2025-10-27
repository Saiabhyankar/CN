#ifndef UDP_FILE_TRANSFER_H
#define UDP_FILE_TRANSFER_H
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>

// Constants
#define MAX_PACKET_SIZE 65507
#define TIMEOUT_SEC 2
#define MAX_RETRIES 5
#define MAX_RECORDS_PER_PACKET 16
#define MAX_SEGMENTS_PER_PACKET 4

// Deterministic loss simulator: 1 lost every LOSS_PERIOD packets at LOSS_OFFSET
// LOSS_PERIOD = 40 -> 1/40 = 2.5% loss
#ifndef LOSS_PERIOD
#define LOSS_PERIOD 40
#endif
#ifndef LOSS_OFFSET
#define LOSS_OFFSET 13
#endif

enum PacketType {
    FILE_HDR = 1,
    FILE_HDR_ACK = 2,
    DATA_PACKET = 3,
    IS_BLAST_OVER = 4,
    REC_MISS = 5,
    DISCONNECT = 6,
    DISCONNECT_ACK = 7
};

struct RecordRange {
    int start;
    int end;
};

struct FileHeader {
    PacketType type;
    long file_size;
    int record_size;
    int blast_size;
    char filename[256];
};

struct BlastQuery {
    PacketType type;
    int m_start;
    int m_end;
};

struct DataPacketHeader {
    PacketType type;
    int num_segments;
    RecordRange segments[MAX_SEGMENTS_PER_PACKET];
};

struct MissingRecordsPacket {
    PacketType type;
    int num_ranges;
    RecordRange ranges[100];
};

struct DisconnectPacket {
    PacketType type;
};

// Utility functions
inline void set_socket_timeout(int sockfd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

inline long get_file_size(const char* filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) return -1;
    return file.tellg();
}

inline bool ensure_directory_exists(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    // try to create directory (mode 0755)
    if (mkdir(path.c_str(), 0755) == 0) return true;
    if (errno == EEXIST) return true;
    return false;
}

#endif // UDP_FILE_TRANSFER_H
