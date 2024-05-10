#ifndef MESSAGE_H
#define MESSAGE_H

#include <iostream>
#include <string>
#include <utility>

enum MessageType {
    VOTE_REQUEST = 0,
    VOTE_RESPONSE,
    LOG_REQUEST,
    LOG_RESPONSE,
    NEW_MESSAGE,
    MESSAGE_ACK
};

struct VoteRequest {
    int candidateId;
    int candidateTerm;
    int logLength;
    int logTerm;

    VoteRequest(int id, int term, int length, int logTerm)
        : candidateId(id), candidateTerm(term), logLength(length), logTerm(logTerm) {}
};

struct VoteResponse {
    int voterId;
    int term;
    bool granted;

    VoteResponse(int id, int t, bool grant)
        : voterId(id), term(t), granted(grant) {}
};

struct LogRequest {
    int leaderId;
    int term;
    int prefixLength;
    int prefixTerm;
    int leaderCommit;
    std::string suffix;

    LogRequest(int id, int t, int plen, int pterm, int commit, std::string suff)
        : leaderId(id), term(t), prefixLength(plen), prefixTerm(pterm), leaderCommit(commit), suffix(std::move(suff)) {}
};

struct LogResponse {
    int followerId;
    int term;
    int ack;
    bool success;

    LogResponse(int id, int t, int a, bool succ)
        : followerId(id), term(t), ack(a), success(succ) {}
};

struct NewMessage {
    int senderId;
    int msgID;
    std::string message;

    NewMessage(int sender, int id, std::string msg)
        : senderId(sender), msgID(id), message(std::move(msg)) {}
};

struct MessageACK {
    int senderId;
    int msgID;

    MessageACK(int sender, int id)
        : senderId(sender), msgID(id) {}
};

#endif // MESSAGE_H
