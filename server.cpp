#include <bits/stdc++.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <endian.h>
#include <thread>
#include <mutex>
#include <chrono>
using namespace std;

#pragma pack(push,1)
struct PacketHdr { uint32_t type, seq, size, totalpkts; };
struct FilePayloadInfo { uint64_t filesize; char path[256]; };
#pragma pack(pop)

enum PacketType { FILE_INFO = 1, FILE_DATA = 2, FILE_END = 3, NACK = 4, ACK_DONE = 5 };
//PARAM TO CHANGE FOR DIFF NETWORKS
const string ETH_INTERFACE = "ens33";
// Get MTU
size_t get_mtu(const string& ETH_INTERFACE) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 1500;
    struct ifreq ifr; strncpy(ifr.ifr_name, ETH_INTERFACE.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFMTU, &ifr) < 0) { close(fd); return 1500; }
    close(fd); return ifr.ifr_mtu;
}

bool first_data = false;
chrono::steady_clock::time_point start_time, end_time;

int main(int argc, char* argv[]) {
    if (argc != 2) { cerr << "Usage: " << argv[0] << " <saveFile>\n"; return 1; }
    string outfile = argv[1];
    size_t mtu_limit = get_mtu(ETH_INTERFACE) - 28;
    cout << "[SERVER] Using MTU payload size: " << mtu_limit << "\n";

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(9000); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    cout << "[SERVER] Listening on port 9000\n";

    uint64_t filesize = 0; uint32_t totalpkts = 0;
    vector<bool> received; mutex file_mutex;
    bool got_end = false;
    sockaddr_in serversock{}; socklen_t sockLen = sizeof(serversock);
    fstream out;

    while (!got_end) {
        char buffer[65536];
        int n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&serversock, &sockLen);
        if (n < (int)sizeof(PacketHdr)) continue;

        PacketHdr hdr; memcpy(&hdr, buffer, sizeof(hdr));
        hdr.type = ntohl(hdr.type); hdr.seq = ntohl(hdr.seq); hdr.size = ntohl(hdr.size); hdr.totalpkts = ntohl(hdr.totalpkts);

        if (hdr.type == FILE_INFO) {
            if (n < (int)(sizeof(PacketHdr) + sizeof(FilePayloadInfo))) continue;
            FilePayloadInfo fileinfo; memcpy(&fileinfo, buffer + sizeof(PacketHdr), sizeof(fileinfo));
            filesize = be64toh(fileinfo.filesize);
            totalpkts = (filesize + mtu_limit - 1) / mtu_limit;
            received.assign(totalpkts, false);

            out.open(outfile, ios::binary | ios::out | ios::trunc);
            out.seekp(filesize - 1); out.write("", 1); out.flush(); out.close();

            cout << "[SERVER] FILE_INFO received: " << outfile << " size=" << filesize << " totalpkts=" << totalpkts << "\n";

            PacketHdr ack{}; ack.type = htonl(ACK_DONE);
            sendto(sock, &ack, sizeof(ack), 0, (sockaddr*)&serversock, sockLen);
        }
        else if (hdr.type == FILE_DATA) {
            thread([&, hdr, n, buffer] {
                lock_guard<mutex> lock(file_mutex);
                if (!first_data) { start_time = chrono::steady_clock::now(); first_data = true; }
                if (hdr.seq < totalpkts && !received[hdr.seq]) {
                    int data_len = min((int)hdr.size, n - (int)sizeof(PacketHdr));
                    fstream out(outfile, ios::binary | ios::in | ios::out);
                    out.seekp(hdr.seq * mtu_limit);
                    out.write(buffer + sizeof(PacketHdr), data_len);
                    out.close();
                    received[hdr.seq] = true;
                }
                }).detach();
        }
        else if (hdr.type == FILE_END) { got_end = true; cout << "[SERVER] FILE_END received\n"; }
    }

    // --- NACK repair loop ---
    const int MAX_RETRY = 200, MAX_SEQ_NACK = 1024;
    for (int retry = 0; retry < MAX_RETRY; retry++) {
        vector<uint32_t> missing;
        { lock_guard<mutex> lock(file_mutex);
        for (uint32_t i = 0; i < totalpkts; i++) if (!received[i]) missing.push_back(i);
        }
        //cout << "[DEBUG] Retry " << retry << ": " << missing.size() << " missing packets\n";

        if (missing.empty()) {
           // cout << "[DEBUG] All packets received, breaking retry loop\n";
            break;
        }

        size_t idx = 0;
        while (idx < missing.size()) {
            size_t batch = min((size_t)MAX_SEQ_NACK, missing.size() - idx);
           // cout << "[DEBUG] Sending NACK batch: " << batch
             //   << " packets, starting at seq " << missing[idx] << "\n";

            uint32_t count_net = htonl((uint32_t)batch);
            vector<char> payload(4 + 4 * batch);
            memcpy(payload.data(), &count_net, 4);
            for (size_t j = 0; j < batch; j++) {
                uint32_t s = htonl(missing[idx + j]);
                memcpy(payload.data() + 4 + j * 4, &s, 4);
            }

            PacketHdr nack_hdr{}; nack_hdr.type = htonl(NACK); nack_hdr.seq = 0;
            nack_hdr.size = htonl((uint32_t)payload.size()); nack_hdr.totalpkts = htonl(totalpkts);
            vector<char> pkt(sizeof(nack_hdr) + payload.size());
            memcpy(pkt.data(), &nack_hdr, sizeof(nack_hdr));
            memcpy(pkt.data() + sizeof(nack_hdr), payload.data(), payload.size());
            ssize_t sent = sendto(sock, pkt.data(), pkt.size(), 0, (sockaddr*)&serversock, sockLen);
           // if (sent < 0) perror("[DEBUG] sendto failed");
            idx += batch;
        }

        auto start = chrono::steady_clock::now();
        while (chrono::duration<double>(chrono::steady_clock::now() - start).count() < 1.0) {
            fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
            timeval tv{ 0,100000 };
            int rv = select(sock + 1, &fds, NULL, NULL, &tv);
            if (rv <= 0) break;
            char buffer[65536]; sockaddr_in from{}; socklen_t flen = sizeof(from);
            int n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&from, &flen);
            if (n < (int)sizeof(PacketHdr)) continue;
            PacketHdr hdr; memcpy(&hdr, buffer, sizeof(hdr));
            hdr.type = ntohl(hdr.type); hdr.seq = ntohl(hdr.seq); hdr.size = ntohl(hdr.size);
            if (hdr.type == FILE_DATA && hdr.seq < totalpkts && !received[hdr.seq]) {
              //  cout << "[DEBUG] Retry " << retry << ": Received FILE_DATA seq=" << hdr.seq
                //    << " size=" << hdr.size << "\n";
                thread([&, hdr, n, buffer] {
                    lock_guard<mutex> lock(file_mutex);
                    int data_len = min((int)hdr.size, n - (int)sizeof(PacketHdr));
                    fstream out(outfile, ios::binary | ios::in | ios::out);
                    out.seekp(hdr.seq * mtu_limit);
                    out.write(buffer + sizeof(PacketHdr), data_len);
                    out.close();
                    received[hdr.seq] = true;
                    }).detach();
            }
        }
    }

    PacketHdr ack{}; ack.type = htonl(ACK_DONE);
    for (int i = 0; i < 10; i++) {
    sendto(sock, &ack, sizeof(ack), 0, (sockaddr*)&serversock, sockLen);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // small delay between sends
    }
    end_time = chrono::steady_clock::now();
    double total_time = chrono::duration<double>(end_time - start_time).count();
    cout << "[SERVER] Total time: " << total_time << " seconds\n";

    close(sock);
    return 0;
}
