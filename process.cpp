#include "process.h"
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <thread>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <utility>
#include <random> 
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <algorithm>
#include <cmath>
#include <fcntl.h>

void safeJoin(std::thread& th) {
    if (th.joinable()) {
        std::thread::id thisId = std::this_thread::get_id();
        if (th.get_id() != thisId) {
            th.join();
        } else {
            th.detach();
        }
    }
}

void setNonBlocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "Failed to get socket flags\n";
        return;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sock, F_SETFL, flags) == -1) {
        std::cerr << "Failed to set non-blocking socket\n";
    }
}

RAFTServer::RAFTServer(int id, int total, int port) 
    : serverId(id), totalServers(total), tcpPort(port), tcpSocket(-1), 
      udpPort(ROOT_PORT + id), udpSocket(-1), running(false), 
      rng(std::chrono::high_resolution_clock::now().time_since_epoch().count() + id),
      dist(FOLLOWER_INTERVAL, 2 * FOLLOWER_INTERVAL),
      lastHeartbeat(std::chrono::steady_clock::now()) {
    timeoutDuration = std::chrono::milliseconds(dist(rng));
}

RAFTServer::~RAFTServer(){
    shutdownServer();
}

void RAFTServer::shutdownServer(){
    running.store(false);
    if (tcpSocket >= 0) {
        close(tcpSocket);
        tcpSocket = -1;
    }

    if (udpSocket >= 0) {
        close(udpSocket);
        udpSocket = -1;
    }

    cv.notify_all();

    safeJoin(proxyThread);
    safeJoin(peerThread);
}

void RAFTServer::waitForThreadsToFinish() {
    if (proxyThread.joinable()) {
        proxyThread.join();
    }
    if (peerThread.joinable()) {
        peerThread.join();
    }
}

void RAFTServer::initializeTCPConnection(){
    tcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpSocket < 0) {
        std::cerr << "Failed to create TCP socket." << std::endl;
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(tcpPort);
    
    if (bind(tcpSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Failed to bind TCP socket." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (listen(tcpSocket, MAX_PEERS) < 0) {
        std::cerr << "Failed to listen on socket." << std::endl;
        exit(EXIT_FAILURE);
    }
}

void RAFTServer::acceptTCPConnections() {
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    while (running.load()) {
        int clientSocket = accept(tcpSocket, (struct sockaddr *)&clientAddr, &clientAddrLen);
        if (clientSocket < 0) {
            std::cerr << "Failed to accept connection." << std::endl;
            continue;
        }
        setNonBlocking(clientSocket);
        handleTCPConnection(clientSocket);
    }
}

void RAFTServer::initializeUDPConnection() {
    struct sockaddr_in udpAddr;
    if ((udpSocket = socket(AF_INET, SOCK_DGRAM, 0))< 0) {
        std::cerr << "Failed to create UDP socket." << std::endl;
        exit(EXIT_FAILURE);
    }
    // Set the socket to non-blocking mode
    int flags = fcntl(udpSocket, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "Error getting flags from UDP socket" << std::endl;
        exit(EXIT_FAILURE);
    }
    flags |= O_NONBLOCK;
    if (fcntl(udpSocket, F_SETFL, flags) == -1) {
        std::cerr << "Error setting UDP socket to non-blocking" << std::endl;
        exit(EXIT_FAILURE);
    }

    memset(&udpAddr, 0, sizeof(udpAddr));
    udpAddr.sin_family = AF_INET;
    udpAddr.sin_addr.s_addr = INADDR_ANY;
    udpAddr.sin_port = htons(udpPort);

    if (bind(udpSocket, (const struct sockaddr *)&udpAddr, sizeof(udpAddr)) < 0) {
        std::cerr << "Failed to bind UDP socket." << std::endl;
        exit(EXIT_FAILURE);
    }
}

bool RAFTServer::sendUDPMessage(int receiverPort, const std::string& message) {
    int sockfd;
    struct sockaddr_in receiverAddr;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        std::cerr << "Socket creation failed: " << strerror(errno) << std::endl;
        return false;
    }

    memset(&receiverAddr, 0, sizeof(receiverAddr));
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_port = htons(receiverPort);
    receiverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (sendto(sockfd, message.c_str(), message.length(), 0, (struct sockaddr *)&receiverAddr, sizeof(receiverAddr)) < 0) {
        std::cerr << "Failed to send message: " << strerror(errno) << std::endl;
        close(sockfd);
        return false;
    }

    close(sockfd);
    return true;
}

std::string RAFTServer::compileChatLog() {
    std::stringstream chatLogStream;

    chatLogStream << "chatLog " ;

    for (int i = 0; i < commitLengthSent; ++i) {
        chatLogStream << log[i].message << ",";
    }

    std::string chatLogString = chatLogStream.str();

    if (!chatLogString.empty()) {
        chatLogString.pop_back();
    }

    return chatLogString;
}

void RAFTServer::printSuffix(const std::vector<LogEntry>& suffix) const {
    std::cout << "Printing Suffix Entries:\n";
    for (const auto& entry : suffix) {
        std::cout << "Term: " << entry.term << ", Msg ID: " << entry.msg_id << ", Message: " << entry.message << std::endl;
    }
    std::cout << "Total entries in suffix: " << suffix.size() << std::endl;
}

std::vector<LogEntry> RAFTServer::parseSuffix(const std::string& suffix) {
    std::vector<LogEntry> entries;
    std::stringstream ss(suffix);
    std::string entry;

    while (std::getline(ss, entry, ';')) {
        if (entry.empty()) continue;

        std::stringstream entryStream(entry);
        std::string part;
        std::vector<std::string> parts;

        while (std::getline(entryStream, part, ',')) {
            parts.push_back(part);
        }

        if (parts.size() == 3) {
            LogEntry logEntry;
            logEntry.term = std::stoi(parts[0]);
            logEntry.msg_id = std::stoi(parts[1]);
            logEntry.message = parts[2];
            entries.push_back(logEntry);
        }
    }
    return entries;
}

void RAFTServer::start(){
    running.store(true);
    proxyThread = std::thread(&RAFTServer::proxyCommunication, this);
    peerThread = std::thread(&RAFTServer::peerCommunication, this);
}

void RAFTServer::proxyCommunication() {
    initializeTCPConnection();
    while (running.load()) {
        acceptTCPConnections();
    }
}

void RAFTServer::resetTimeout() {
    lastHeartbeat = std::chrono::steady_clock::now();
    if (currentRole == FOLLOWER) {
        timeoutDuration = std::chrono::milliseconds(dist(rng));
    } else if (currentRole == CANDIDATE) {
        timeoutDuration = std::chrono::milliseconds(ELECTION_INTERVAL + rand() % ELECTION_INTERVAL);
    } else if (currentRole == LEADER) {
        timeoutDuration = std::chrono::milliseconds(HEARTBEAT_INTERVAL);
    }
}

void RAFTServer::handleTCPConnection(int clientSocket) {
    std::string receivedData;
    char buffer[MAX_BUFFER_SIZE];
    bool keepListening = true;

    while (keepListening) {

        /* send back ack when new message committed */
        if (commitLength.load() > commitLengthSent.load()){
            if (commitLengthSent.load() < static_cast<int>(log.size())){
                const auto& entry = log[commitLengthSent.load()];
                std::string message = "ack " + std::to_string(entry.msg_id) + " " + std::to_string(commitLengthSent.load()) + "\n";
                send(clientSocket, message.c_str(), message.size(), 0);
                commitLengthSent++;
            }
        }

        ssize_t bytesRead = read(clientSocket, buffer, MAX_BUFFER_SIZE - 1);
        if (bytesRead < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
                std::cerr << "Error reading from socket: " << strerror(errno) << std::endl;
                break;
            }
        } else if (bytesRead == 0) {
            break;
        } else {
            buffer[bytesRead] = '\0';
            receivedData.append(buffer);
            size_t pos;
            while ((pos = receivedData.find('\n')) != std::string::npos) {
                std::string command = receivedData.substr(0, pos);
                receivedData.erase(0, pos + 1);
                keepListening = processCommand(command, clientSocket);
                if (!keepListening) {
                    break;
                }
            }
        }
    }
    if (clientSocket >= 0) {
        close(clientSocket);
    }
}

void RAFTServer::replicateLog(int leaderId, int followerId){
    int prefixLen = sentLength[followerId];
    int prefixTerm = 0;
    if (prefixLen > 0) {
        prefixTerm = log[prefixLen - 1].term;
    }

    std::vector<LogEntry> suffix(log.begin() + prefixLen, log.end());

    std::ostringstream suffixStream;
    for (const auto& entry : suffix) {
        suffixStream << entry.term << "," << entry.msg_id << "," << entry.message << ";";
    }
    std::string suffix_str = suffixStream.str();

    LogRequest logReq(leaderId, currentTerm, prefixLen, prefixTerm, commitLength, suffix_str);

    std::ostringstream messageStream;
    messageStream << static_cast<int>(LOG_REQUEST) << " "
                  << logReq.leaderId << " "
                  << logReq.term << " "
                  << logReq.prefixLength << " "
                  << logReq.prefixTerm << " "
                  << logReq.leaderCommit << " "
                  << logReq.suffix;

    sendUDPMessage(ROOT_PORT + followerId, messageStream.str());
}

void RAFTServer::printLogEntries() {
    std::cout << "printLogEntries() for serverid:" << serverId << " commitedLenth:" << commitLength << " commitedLengthSent:" << commitLengthSent << std::endl;
    std::cout << "Current Log Entries at serverId:" << serverId << ": ";
    for (const auto& entry : log) {
        std::cout << "Term: " << entry.term << ", Message ID: " << entry.msg_id << ", Message: " << entry.message << "///////";
    }
    std::cout << std::endl;
}

void RAFTServer::printMessageQueue() {
    std::queue<ClientMsg> copyQueue = messageQueue;
    std::cout << "Current Message Queue State at ServerId: " << serverId << ":";
    if (copyQueue.empty()) {
        std::cout << "Empty" << std::endl;
    } else {
        while (!copyQueue.empty()) {
            const ClientMsg& msg = copyQueue.front();
            std::cout << "Message ID: " << msg.msg_id << " Message: " << msg.message << std::endl;
            copyQueue.pop();
        }
    }
}

bool RAFTServer::processCommand(const std::string& command, int clientSocket){
    if (command.substr(0, 4) == "msg ") {
        try {
            size_t messageIdEnd = command.find(' ', 4);
            if (messageIdEnd != std::string::npos) {
                int msgId = std::stoi(command.substr(4, messageIdEnd - 4));
                std::string messageText = command.substr(messageIdEnd + 1);
                ClientMsg msg = {msgId, messageText};
                messageQueue.push(msg);
            } else {
                std::cerr << "Error: No space after message ID, invalid command format" << std::endl;
            }
        } catch (const std::invalid_argument& e) {
            std::cerr << "Error: Invalid message ID" << std::endl;
        } catch (const std::out_of_range& e) {
            std::cerr << "Error: Message ID out of range" << std::endl;
        }
        return true;
    } else if (command == "get chatLog") {
        /* Send back committed log */
        std::string chatLog = compileChatLog();

        if (chatLog.empty() || chatLog == "chatLog") {
            std::string message = "chatLog <Empty>\n";
            send(clientSocket, message.c_str(), message.size(), 0);
        } else {
            chatLog += "\n";
            size_t totalSent = 0;
            while (totalSent < chatLog.size()) {
                ssize_t sent = send(clientSocket, chatLog.c_str() + totalSent, chatLog.size() - totalSent, 0);
                if (sent < 0) {
                    std::cerr << "Error sending chat log: " << strerror(errno) << std::endl;
                    break;
                }
                totalSent += sent;
            }
        }
        return true;
    } else if (command == "crash") {
        std::cout << "serverId:" << serverId << " crash with role: " << currentRole << std::endl;
        shutdownServer();
        return false;
    } else {
        std::cerr << "Unknown command received: " << command << std::endl;
        return true;
    }
}


void RAFTServer::commitLogEntries(){
    while (static_cast<size_t>(commitLength) < log.size()) {
        int acks = 1;
        for (int i = 0; i < totalServers; i++) {
            if (i != serverId){
                if (ackedLength[i] >= commitLength){
                    acks++;
                }
            }
        }
        if (acks >= ceil((totalServers + 1) / 2)){
            commitLength++;
        } else {
            break;
        }
    }
}

void RAFTServer::peerCommunication(){
    initializeUDPConnection();

    struct sockaddr_in cliaddr;
    memset(&cliaddr, 0, sizeof(cliaddr));
    socklen_t len = sizeof(cliaddr);
    char buffer[MAX_BUFFER_SIZE];

    while (running.load()) {

        ssize_t bytesRead = recvfrom(udpSocket, (char *)buffer, MAX_BUFFER_SIZE, 0, (struct sockaddr *) &cliaddr, &len);

        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            std::string message(buffer);
            handlePeerMessage(message);
        }

        switch (currentRole){
            case FOLLOWER:{
                /* send NEW_MESSAGE if messageQueue not empty to leader */
                if (!messageQueue.empty() && currentLeader != -1){
                    ClientMsg msg = messageQueue.front();
                    sendNewMessage(msg);
                }
                break;
            }
            case CANDIDATE:
                break;
            case LEADER:{
                if (!messageQueue.empty() && messageQueue.front().msg_id == static_cast<int>(log.size())){
                    std::cout << " LEADER at serverId:" << serverId << " push into log for msg_id:" << messageQueue.front().msg_id << std::endl;
                    ClientMsg msg = messageQueue.front();
                    messageQueue.pop();
                    log.push_back({currentTerm.load(), msg.msg_id, msg.message});
                    for (int i = 0; i < totalServers; i++) {
                        if (i != serverId){
                            replicateLog(serverId, i);
                        }
                    }
                } else if (!messageQueue.empty() && messageQueue.front().msg_id < static_cast<int>(log.size())){
                    std::cout << " LEADER at serverId:" << serverId << " !messageQueue.empty() && messageQueue.front().msg_id < static_cast<int>(log.size())) pop" << messageQueue.front().msg_id << std::endl;
                    messageQueue.pop();
                }
                break;
            }
        }

        if (std::chrono::steady_clock::now() - lastHeartbeat > timeoutDuration) {
            handleTimeout();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RAFTServer::handleTimeout() {
    switch (currentRole) {
        case FOLLOWER:
            std::cout << "TIMEOUT! at serverID: " << serverId << " currentRole: FOLLOWER, run ELECTION!" << std::endl;
            runElection();
            break;
        case CANDIDATE:
            currentTerm = currentTerm - 1;
            runElection();
            break;
        case LEADER:
            for (int i = 0; i < totalServers; i++) {
                if (i != serverId){
                    replicateLog(serverId, i);
                }
            }
            break;
    }
}

void RAFTServer::appendEntries(int prefixLen, int leaderCommit, std::vector<LogEntry> suffix) {
    if ((suffix.size() > 0) && (static_cast<size_t>(log.size()) > static_cast<size_t>(prefixLen))) {
        int index = std::min(static_cast<int>(log.size()), prefixLen + static_cast<int>(suffix.size())) - 1;
        if (log[index].term != suffix[index - prefixLen].term) {
            if (prefixLen > 0 && static_cast<size_t>(prefixLen) <= log.size()) {
                log.erase(log.begin() + prefixLen, log.end());
            }
        }
    }
    if (prefixLen + suffix.size() > log.size()){
        for (size_t i = log.size() - prefixLen; i <= suffix.size() - 1; i++) {
            log.push_back(suffix[i]);
        }
    }
    if (leaderCommit > commitLength){
        commitLength = leaderCommit;
        if (!messageQueue.empty() && messageQueue.front().msg_id < leaderCommit){
            messageQueue.pop();
        }
    }
}

void RAFTServer::runElection() {
    currentTerm = currentTerm + 1;
    currentRole = CANDIDATE;
    votedFor = serverId;
    votesReceived.clear();
    votesReceived.insert(serverId);

    int lastTerm = 0;
    int lastLogIndex = log.size() - 1;

    if (log.size() > 0){
        lastTerm = log[lastLogIndex].term;
    }
    
    VoteRequest voteReq(serverId, currentTerm, log.size(), lastTerm);
    
    std::ostringstream messageStream;
    messageStream << static_cast<int>(VOTE_REQUEST) << " "
                  << voteReq.candidateId << " "
                  << voteReq.candidateTerm << " "
                  << voteReq.logLength << " "
                  << voteReq.logTerm;

    for (int i = 0; i < totalServers; i++) {
        if (i != serverId){
            sendUDPMessage(ROOT_PORT + i, messageStream.str());
        }
    }

    resetTimeout();
}


void RAFTServer::handlePeerMessage(const std::string& message) {
    std::istringstream stream(message);
    int messageType;
    stream >> messageType;

    switch (messageType) {
        case VOTE_REQUEST: {
            int cId, cTerm, cLogLength, cLogTerm;
            stream >> cId >> cTerm >> cLogLength >> cLogTerm;
            if (cTerm > currentTerm){
                currentTerm = cTerm;
                currentRole = FOLLOWER;
                votedFor = -1;
                resetTimeout();
            }
            int lastTerm = 0;
            int lastLogIndex = log.size() - 1;
            if (log.size() > 0){
                lastTerm = log[lastLogIndex].term;
            }
            bool logOk = false;
            if ((cLogTerm > lastTerm) || ((cLogTerm == lastTerm) && (cLogLength >= static_cast<int>(log.size())))) {
                logOk = true;
            }
            if ((cTerm == currentTerm) && logOk && ((votedFor == -1) || (votedFor == cId))){
                votedFor = cId;
                VoteResponse voteRsp(serverId, currentTerm, true);
                std::ostringstream messageStream;
                messageStream << static_cast<int>(VOTE_RESPONSE) << " "
                            << voteRsp.voterId << " "
                            << voteRsp.term << " "
                            << voteRsp.granted;
                            
                sendUDPMessage(ROOT_PORT + cId, messageStream.str());

                // reset the timer
                currentRole = FOLLOWER;
                resetTimeout();
            } else {
                VoteResponse voteRsp(serverId, currentTerm, false);
                std::ostringstream messageStream;
                messageStream << static_cast<int>(VOTE_RESPONSE) << " "
                            << voteRsp.voterId << " "
                            << voteRsp.term << " "
                            << voteRsp.granted;
                            
                sendUDPMessage(ROOT_PORT + cId, messageStream.str());
            }
            break;
        }
        case VOTE_RESPONSE: {
            int voterId, term;
            bool granted;
            stream >> voterId >> term >> granted;
            if ((currentRole == CANDIDATE) && (term == currentTerm) && granted){
                votesReceived.insert(voterId);
                if (votesReceived.size() >= ceil((totalServers + 1) / 2)){
                    currentRole = LEADER;
                    currentLeader = serverId;
                    resetTimeout();
                    std::cout << "serverId:" << serverId << " is now the new LEADER" << std::endl;
                    for (int i = 0; i < totalServers; i++) {
                        if (i != serverId){
                            sentLength[i] = log.size();
                            ackedLength[i] = 0;
                            replicateLog(serverId, i);
                        }
                    }
                }
            } else if (term > currentTerm){
                currentTerm = term;
                currentRole = FOLLOWER;
                votedFor = -1;
                resetTimeout();
            }
            break;
        }
        case LOG_REQUEST: {
            int leaderId, term, prefixLen, prefixTerm, leaderCommit;
            std::string suffix_str;
            stream >> leaderId >> term >> prefixLen >> prefixTerm >> leaderCommit;
            std::getline(stream, suffix_str);
            suffix_str.erase(0, suffix_str.find_first_not_of(" \t\n\v\f\r"));
            std::vector<LogEntry> suffix = parseSuffix(suffix_str);
            if (term > currentTerm){
                currentTerm = term;
                votedFor = -1;
                resetTimeout();
            }
            if (term == currentTerm){
                currentRole = FOLLOWER;
                votedFor = -1;
                currentLeader = leaderId;
                resetTimeout();
            }
            bool logOk = false;
            if ((static_cast<size_t>(log.size()) >= static_cast<size_t>(prefixLen)) && ((prefixLen == 0) || (log[prefixLen - 1].term == prefixTerm))) {
                logOk = true;
            }
            if ((term == currentTerm) && logOk){
                appendEntries(prefixLen, leaderCommit, suffix);
                int ack = prefixLen + suffix.size();
                LogResponse logRsp(serverId, currentTerm, ack, true);
                std::ostringstream messageStream;
                messageStream << static_cast<int>(LOG_RESPONSE) << " "
                            << logRsp.followerId << " "
                            << logRsp.term << " "
                            << logRsp.ack << " "
                            << logRsp.success;
                sendUDPMessage(ROOT_PORT + leaderId, messageStream.str());
            } else {
                LogResponse logRsp(serverId, currentTerm, 0, false);
                std::ostringstream messageStream;
                messageStream << static_cast<int>(LOG_RESPONSE) << " "
                            << logRsp.followerId << " "
                            << logRsp.term << " "
                            << logRsp.ack << " "
                            << logRsp.success;
                sendUDPMessage(ROOT_PORT + leaderId, messageStream.str());
            }
            break;
        }
        case LOG_RESPONSE: {
            int follower, term, ack;
            bool success;
            stream >> follower >> term >> ack >> success;
            if ((term == currentTerm) && (currentRole == LEADER)){
                if (success && (ack >= ackedLength[follower])){
                    sentLength[follower] = ack;
                    ackedLength[follower] = ack;
                    commitLogEntries();
                } else if (sentLength[follower] > 0){
                    sentLength[follower] = sentLength[follower] - 1;
                    replicateLog(serverId, follower);
                }
            } else if (term > currentTerm){
                currentTerm = term;
                currentRole = FOLLOWER;
                votedFor = -1;
                resetTimeout();
            }
            break;
        }
        case NEW_MESSAGE: {
            int senderId, msgID;
            std::string msgText;
            stream >> senderId >> msgID;
            std::getline(stream, msgText);
            msgText.erase(0, msgText.find_first_not_of(" \t\n\v\f\r"));
            if (msgID == static_cast<int>(log.size())){
                log.push_back({currentTerm.load(), msgID, msgText});
                std::ostringstream messageStream;
                messageStream << static_cast<int>(MESSAGE_ACK) << " "
                                << serverId << " "
                                << msgID;
                sendUDPMessage(ROOT_PORT + senderId, messageStream.str());
            } else if (msgID < static_cast<int>(log.size())){
                std::ostringstream messageStream;
                messageStream << static_cast<int>(MESSAGE_ACK) << " "
                                << serverId << " "
                                << msgID;
                sendUDPMessage(ROOT_PORT + senderId, messageStream.str());
            }
            break;
        }
        case MESSAGE_ACK: {
            int senderId, msgID;
            stream >> senderId >> msgID;
            // std::cout << "MESSAGE_ACK at serverId:" << serverId << " senderId:" << senderId << " msgId:" << msgID << std::endl;
            // if (!messageQueue.empty() && messageQueue.front().msg_id == msgID){
            //     std::cout << "MESSAGE_ACK at serverId:" << serverId << " senderId:" << senderId << " msgId:" << msgID << " !POP!" << std::endl;
            //     messageQueue.pop();
            // }
            break;
        }
        default:
            std::cerr << "Unknown message type received: " << messageType << std::endl;
    }
}

void RAFTServer::sendNewMessage(const ClientMsg& msg) {
    if (messageQueue.empty()) {
        std::cerr << "Error: No message to send." << std::endl;
        return;
    }

    if (currentRole != FOLLOWER) {
        std::cerr << "Error: Only followers should send new messages to the leader." << std::endl;
        return;
    }

    if (currentLeader == -1) {
        std::cerr << "Error: No leader is known to forward the message." << std::endl;
        return;
    } 

    std::ostringstream messageStream;
    messageStream << static_cast<int>(NEW_MESSAGE) << " " << serverId << " " << msg.msg_id << " " << msg.message;

    int leaderPort = ROOT_PORT + currentLeader;
    if (!sendUDPMessage(leaderPort, messageStream.str())) {
        std::cerr << "Failed to send new message to the leader at port: " << leaderPort << std::endl;
    }
}

int main(int argc, char* argv[]) {

    /* Check Arguments */
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <pid> <n> <port>" << std::endl;
        return EXIT_FAILURE;
    }

    int index = std::atoi(argv[1]);
    int total_server = std::atoi(argv[2]);
    int tcp_port = std::atoi(argv[3]);

    /* Start Server */
    RAFTServer server(index, total_server, tcp_port);
    server.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.waitForThreadsToFinish();

    return EXIT_SUCCESS;
}
