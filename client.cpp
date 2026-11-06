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

//PARAMS TO CHANGE FOR DIFF NETWORKS
const string SERVER_IP = "192.168.10.100";   
const string ETH_INTERFACE = "ens33";            


// Get MTU of an interface
size_t get_mtu(const string& ETH_INTERFACE) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    //cout << "fd" << fd << "\n";

    if (fd < 0) return 1500;
    struct ifreq ifr;
    strncpy(ifr.ifr_name, ETH_INTERFACE.c_str(), IFNAMSIZ - 1);
    //cout << "[CLIENT] ioctl(fd, SIOCGIFMTU, &ifr) " << ioctl(fd, SIOCGIFMTU, &ifr) << " ifr.ifr_mtu=" << ifr.ifr_mtu << "\n";
    if (ioctl(fd, SIOCGIFMTU, &ifr) < 0) { close(fd); return 1500; }
    close(fd);
    //cout << "[DEBUG] MTU of " << ETH_INTERFACE << " = " << ifr.ifr_mtu << "\n";
    return ifr.ifr_mtu;
}

int main(int argc, char* argv[]) {
    if (argc != 2) { cerr << "Usage: " << argv[0] << " <sourceFile>\n"; return 1; }
    string sourceFile = argv[1];
    

    // Determine MTU dynamically
    size_t mtu_limit = get_mtu(ETH_INTERFACE) - 28;
    size_t max_threads = (mtu_limit > 2000) ? 4 : 8;
    cout << "[CLIENT] Using MTU payload size: " << mtu_limit << "\n";

    // Read file into chunks
    ifstream in(sourceFile, ios::binary);
    if (!in) { cerr << "Cannot open " << sourceFile << "\n"; return 1; }
    in.seekg(0, ios::end); size_t filesize = in.tellg(); in.seekg(0);
    size_t totalpkts = (filesize + mtu_limit - 1) / mtu_limit;

    vector<string> chunks(totalpkts);
    for (size_t i = 0; i < totalpkts; i++) {
        size_t len = min(mtu_limit, filesize - i * mtu_limit);
        chunks[i].resize(len); in.read(&chunks[i][0], len);
    }
    in.close();

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in clientsock{}; clientsock.sin_family = AF_INET; clientsock.sin_port = htons(9000);
    inet_pton(AF_INET, SERVER_IP.c_str(), &clientsock.sin_addr);

    fd_set fds; sockaddr_in from{}; socklen_t sockLen = sizeof(from);
    mutex send_mutex;

    // --- handshake: FILE_INFO ---
    FilePayloadInfo fileinfo{}; fileinfo.filesize = htobe64(filesize);
    string basename = sourceFile.substr(sourceFile.find_last_of("/\\") + 1);
    strncpy(fileinfo.path, basename.c_str(), sizeof(fileinfo.path) - 1);

    PacketHdr fhdr{}; fhdr.type = htonl(FILE_INFO); fhdr.seq = 0;
    fhdr.size = htonl(sizeof(fileinfo)); fhdr.totalpkts = htonl(totalpkts);

    vector<char> pkt(sizeof(fhdr) + sizeof(fileinfo));
    memcpy(pkt.data(), &fhdr, sizeof(fhdr));
    memcpy(pkt.data() + sizeof(fhdr), &fileinfo, sizeof(fileinfo));

    int retry = 0; bool handshake = false; const int MAX_RETRIES = 20;
    while (retry < MAX_RETRIES && !handshake) {
        sendto(sock, pkt.data(), pkt.size(), 0, (sockaddr*)&clientsock, sizeof(clientsock));
        FD_ZERO(&fds); FD_SET(sock, &fds); timeval tv{ 5,0 };
        int rv = select(sock + 1, &fds, NULL, NULL, &tv);
        if (rv > 0) {
            char buffer[1024]; int n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&from, &sockLen);
            if (n >= (int)sizeof(PacketHdr)) {
                PacketHdr hdr; memcpy(&hdr, buffer, sizeof(hdr));
                if (ntohl(hdr.type) == ACK_DONE) { handshake = true; break; }
            }
        }
        retry++;
    }
    if (!handshake) { cerr << "[CLIENT] Handshake failed\n"; return 1; }
    cout << "[CLIENT] Handshake done\n";

    // --- send packets ---
    auto send_packet = [&](uint32_t seq) {
        PacketHdr packhead{}; packhead.type = htonl(FILE_DATA); packhead.seq = htonl(seq);
        packhead.size = htonl(chunks[seq].size()); packhead.totalpkts = htonl(totalpkts);

        vector<char> packvec(sizeof(packhead) + chunks[seq].size());
        memcpy(packvec.data(), &packhead, sizeof(packhead));
        memcpy(packvec.data() + sizeof(packhead), chunks[seq].data(), chunks[seq].size());

        lock_guard<mutex> lock(send_mutex);
        sendto(sock, packvec.data(), packvec.size(), 0, (sockaddr*)&clientsock, sizeof(clientsock));
    };

    vector<thread> threadworkers;
    auto start_time = chrono::steady_clock::now();
    for (uint32_t i = 0; i < totalpkts; i++) {
        threadworkers.emplace_back(send_packet, i);
        if (threadworkers.size() >= max_threads) {
            for (auto& t : threadworkers) t.join();
            threadworkers.clear();
        }
    }
    for (auto& t : threadworkers) t.join();

    // --- FILE_END ---
    PacketHdr end{}; end.type = htonl(FILE_END); 
    for (int i = 0; i < 10; i++) {
        sendto(sock, &end, sizeof(end), 0, (sockaddr*)&clientsock, sizeof(clientsock));
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // small delay between sends
    }
    cout << "[CLIENT] Sent FILE_END marker\n";
    // --- NACK/resend loop ---
    bool finished = false;
    while (!finished) {
        FD_ZERO(&fds); FD_SET(sock, &fds); timeval tv2{ 200,0 };
        int rv = select(sock + 1, &fds, NULL, NULL, &tv2);
        if (rv <= 0) { cout << "[CLIENT] Timeout\n"; break; }

        char buffer[65536]; int n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&from, &sockLen);
        if (n < (int)sizeof(PacketHdr)) continue;
        PacketHdr hdr; memcpy(&hdr, buffer, sizeof(hdr)); hdr.type = ntohl(hdr.type);

        if (hdr.type == NACK) {
            if (n < (int)(sizeof(PacketHdr) + 4)) continue;
            uint32_t countnet; memcpy(&countnet, buffer + sizeof(PacketHdr), 4); uint32_t count = ntohl(countnet);
            if ((int)(sizeof(PacketHdr) + 4 + count * 4) > n) continue;
            vector<thread> nackthread;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t seq; memcpy(&seq, buffer + sizeof(PacketHdr) + 4 + i * 4, 4); seq = ntohl(seq);
                nackthread.emplace_back(send_packet, seq);
            }
            for (auto& t : nackthread) t.join();
        }
        else if (hdr.type == ACK_DONE) { finished = true; break; }
    }

    auto end_time = chrono::steady_clock::now();
    double total_time = chrono::duration<double>(end_time - start_time).count();
    cout << "[CLIENT] Total time: " << total_time << " seconds\n";

    close(sock);
    return 0;
}
