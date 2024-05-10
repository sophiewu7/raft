#ifndef PROCESS_H
#define PROCESS_H

#include "message.h"
#include <vector>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <random>
#include <set>

constexpr int ROOT_PORT = 20000;
constexpr int MAX_PEERS = 5;
constexpr int MAX_BUFFER_SIZE = 1024;
constexpr int ELECTION_INTERVAL = 500;
constexpr int HEARTBEAT_INTERVAL = 300;
constexpr int FOLLOWER_INTERVAL = 1000;

enum ServerStatus {
    FOLLOWER = 0,
    CANDIDATE,
    LEADER
};

struct LogEntry {
    int term;
    int msg_id;
    std::string message;
};

struct ClientMsg {
    int msg_id;
    std::string message;
};

class RAFTServer {
private:
    int serverId;
    int totalServers;
    int tcpPort;
    int tcpSocket;
    int udpPort;
    int udpSocket;
    std::atomic<bool> running;

    std::atomic<int> currentTerm{0};
    std::atomic<int> votedFor{-1};
    std::vector<LogEntry> log;
    std::atomic<int> commitLength{0};
    std::atomic<int> commitLengthSent{0};
    std::atomic<ServerStatus> currentRole{FOLLOWER};
    std::atomic<int> currentLeader{-1};
    std::set<int> votesReceived;
    std::map<int, int> sentLength;
    std::map<int, int> ackedLength;
    
    std::queue<ClientMsg> messageQueue;
    
    std::mutex mutex;
    std::condition_variable cv;

    std::thread proxyThread;
    std::thread peerThread;

    std::mt19937 rng;
    std::uniform_int_distribution<int> dist;
    std::chrono::steady_clock::time_point lastHeartbeat;
    std::chrono::milliseconds timeoutDuration;

    std::mutex messageQueueMutex;

public:

    RAFTServer(int serverId, int totalServers, int tcpPort);
    void start();
    void waitForThreadsToFinish();

    ~RAFTServer();
    void shutdownServer();
    
    void proxyCommunication();
    void initializeTCPConnection();
    void acceptTCPConnections();
    void handleTCPConnection(int clientSocket);
    bool processCommand(const std::string& command, int clientSocket);
    void printMessageQueue();

    void peerCommunication();
    void initializeUDPConnection();
    void handlePeerMessage(const std::string& message);
    bool sendUDPMessage(int receiverPort, const std::string& message);

    void sendNewMessage(const ClientMsg& msg);

    void handleTimeout();

    void runElection();
    void replicateLog(int leaderId, int followerId);
    std::vector<LogEntry> parseSuffix(const std::string& suffix);
    void appendEntries(int prefixLen, int leaderCommit, std::vector<LogEntry> suffix);
    void commitLogEntries();

    std::string compileChatLog();
    void resetTimeout();
    void printLogEntries();
    void printSuffix(const std::vector<LogEntry>& suffix) const;
};

#endif // PROCESS_H
