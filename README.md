# CS5450 - Network and Distributed System - Raft

## Introduction

In this project, we have developed a distributed chat application utilizing the Raft consensus algorithm. The primary aim is to explore and practically implement the principles of distributed systems with an emphasis on ensuring consistent message ordering across various nodes, even in the presence of node or network failures. 

## Implementation Detail

### Overall Architecture

The overall architecture of the Raft-based distributed chat application system consist of serveral key components:

1. **Server Nodes**: Client can start serveral process to server as node via proxy.py. Each server node runs an instance of `RAFTServer` class, which implements the functionality necessary to participate in the Raft consensus protocol. These nodes are responsible for maintaining the replicated log, processing client requests, and performing leader election.
2. **TCP and UDP Sockets**: The system utlizes TCP sockets for stable connections where reliability is crucial (eg. receiving messages from and sending messages to clients) and UDP sockets for intra-cluster communication among nodes, which involves sending and receiving logs and voting message. 
3. **Threads**: Multiple threads handle different aspects of the system:
    * **Proxy Thread**: Manages TCP connections with clients to receive new chat messages.
    * **Peer Communication Thread**: Handles UDP-based communications between nodes, and deals with the logic of Raft protocol, including leader election, log replication, timeout handling, and so on. 
    * **Main Thread**: Orchestrates the initialization and shutdown processes, and oversees the main server operations.


### Implementation of Raft Protocol

#### Node State Transitions in Raft
In the Raft consensus algorithm, each node in the cluster can be in one of three states: Follower, Candidate, or Leader. These states determine the node's role and behavior within the cluster. 

![image](https://hackmd.io/_uploads/SybYYqqzC.png)
The figure above is adapted from the [Martin Kleppmann](https://martin.kleppmann.com/2020/11/18/distributed-systems-and-elliptic-curves.html)

1. **Follower**:
    * **Initial State**: Nodes start as Followers. This is their default state.
    * **Behavior**: Followers are mostly passive. They respond to requests from Leaders and Candidates. They respond to the Leader’s log entries by replicating the log commands sent by the Leader and vote for Candidates during an election.
    * **Transitions**:
        * If a Follower does not hear from a Leader within a certain period (follower timeout), it assumes there is no active Leader and transitions to the Candidate state to start an election.
        * To avoid split votes and ensure smooth transitions during elections, the follower timeout is randomized. This means that each follower waits for a slightly different amount of time before deciding to become a Candidate, which helps reduce the likelihood of multiple Followers becoming Candidates at the exact same time. 

2. **Candidate**:
    * **Trigger**: This state is triggered when the election timeout expires without receiving a heartbeat from the Leader.
    * **Behavior**: Candidates request votes from other nodes. They increment their term and vote for themselves first, then send out a request for votes to other nodes. If the it does not receive enough votes for a certain period (election timeout), it will still be the candidate, restart a new round of election, and resend `VOTE_REQUEST`.
    * **Transitions**:
        * **To Leader**: If the Candidate receives a majority of votes from the quorum, it transitions to a Leader.
        * **Back to Follower**: If the Candidate discovers a new term (i.e., a new Leader is elected, or it receives a log entry from a new Leader), or if the election is split and a new term starts, it returns to the Follower state.

3. **Leader**:
    * **Trigger**: A node becomes a Leader when it receives the majority of votes from the quorum in the current term.
    * **Behavior**: The Leader handles all client requests, sends`LOG_REQUEST` with non-empty `suffix` when there is new message. When there is no new message, the leader sends`LOG_REQUEST` with empty `suffix` to serve as heartbeats to tell Followers that the leader is still active. In this way leader is able to maintain authority and prevent new elections, and is responsible for log replication across the cluster.
    * Transitions:
        * **Back to Follower:** If the Leader discovers a higher term (either through communication from another node that has advanced to a higher term or other disruptions), it will step down and revert to the Follower state.

### Raft Consensus Operations
* **Leader Election**:
    * This part is illustrated detailedly above.
* **Log Replication**: 
    * The Leader handles log entries by appending new entries received from clients to its log and then replicating these entries across all Follower nodes.
    * Followers append these entries to their logs only if they match the Leader’s log up to the point of new entries (consistency check).
    * If a Follower’s log is inconsistent or incomplete compared to the Leader’s, the Leader will decrement the next index for that Follower and retry log replication until successful.
    * Once a new log entry is stored by a majority of the servers, the entry is considered committed. The Leader then notifies Followers of the newly committed entries to apply them to their state machines.
    * When the client send `get chatLog` command to the server via proxy, the server will return all the committed entries back to the client. 


* **Handling Failures and Safety**: 
    * **Follower Timeout**: If the Follower does not receive a log request or heartbeat from the Leader within a set timeout period, it assumes that there is currently no valid leader and converts itself into a Candidate to initiate a new leader election.
    * **Log consistency**: If log inconsistencies are found (for example, a follower's log is behind or wrong), the leader resends missing or inconsistent log entries to fix those problems. This ensures that all nodes in the cluster have complete and consistent logs.
    * **Term Numbering**: Raft uses term to prevent obsolete leaders from interfering with the cluster. Each term, nodes may vote for one candidate. If a request is received from an old term, it will be ignored.
    * **Committed Index**: The leader maintains a global commit index that represents all log entries that have been securely copied to most nodes. A log entry is considered "committed" only if a majority of nodes have copied it. 

### Data Structure

#### Primary Data Structures

**Atomic Variables:**
* `std::atomic<bool> running`: Controls the server's main loop and its execution state.
* `std::atomic<int> currentTerm`, `votedFor`, `commitLength`, `commitLengthSent`, `currentRole`, `currentLeader`: These atomic integers store the current term of the server, the candidate voted for, the length of committed entries, the length of sent committed entries, the server's current role (follower, candidate, leader), and the identifier of the current leader. These variables are crucial for ensuring consistency and synchronization across server states without locking overhead.

**Vectors and Maps:**

* `std::vector<LogEntry> log`: A dynamic array that stores the log entries. Each entry contains a term number, a unique message identifier, and the message itself. This is essential for the RAFT consensus algorithm.
* `std::map<int, int> sentLength, ackedLength`: Maps that track the number of entries sent to and acknowledged by each peer, respectively. These are vital for log replication and ensuring data consistency across the cluster.

**Thread Management:**
* `std::thread proxyThread, peerThread`: Threads for handling different aspects of server operation—one for client interactions over TCP and another for peer communication over UDP.

**Message Queue:**
* `std::queue<ClientMsg> messageQueue`: A FIFO queue holding messages from clients that need to be processed. This queue decouples message reception from processing, allowing efficient management of incoming commands. The follower only pop message out of the queue once they found the message is committed in the log or the message is outdated (msgId < commitLength).


#### Message Types

The provided `message.h` header file defines six types of messages. Each of these message structures is designed to support specific interactions within the Raft protocol:

* **Election Process**: `VoteRequest` and `VoteResponse` messages are exchanged during the election process to elect a new leader. These messages ensure that only a candidate with a log as up-to-date as the majority's logs can be elected.
* **Log Replication**: `LogRequest` and `LogResponse` enable the leader to replicate its log across all followers and ensure consistency. If discrepancies arise, the leader adjusts its requests to help the follower catch up.
* **Message Forward Interaction**: `NewMessage` used for followers to forward message received from proxy.py / client to leader.

### `proxy.py` Modification

We changed `line 53` in `proxy.py` to
```python
self.buffer += data.decode('utf-8')
```

Modify proxy.py line 75 to 
```python
self.sock.send((str(s) + '\n').encode('utf-8'))
```

**Explanation:** We found that in Python 3, `socket.recv()` returns data as a bytes object, not a str object. To concatenate it to self.buffer, which is a string, you need to decode data from bytes to a string using the `decode()` method, which uses `UTF-8` encoding by default.

We also modify the timeout in proxy from 120 to 1200.


## Testing 

### Running Environment

* Ubuntu 22.04.3 LTS
* macOS Sonoma 14.3 with Chip Apple M1 Pro

Our Makefile uses C++17. Please ensure that your machine's compiler is up-to-date. Otherwise, there may be issues with the libraries.

### Notice
For `get chatLog`, if the server has nothing to return, the output would be `<Empty>` instead of nothing. 

### Test Results

#### Provided Testcases
`functional.input`
```bash=
sophiewu7@Pro7:~/network/a4$ python proxy.py
0 start 4 10000
1 start 4 10001
2 start 4 10002
3 start 4 10003
0 msg 0 WhatsYourName
-1 waitForAck 0
1 msg 1 Alice
2 msg 2 Bob
3 msg 3 Carol
-1 waitForAck 1
-1 waitForAck 2
-1 waitForAck 3
0 get chatLog
WhatsYourName,Alice,Bob,Carol
1 get chatLog
WhatsYourName,Alice,Bob,Carol
2 get chatLog
WhatsYourName,Alice,Bob,Carol
3 get chatLog
WhatsYourName,Alice,Bob,Carol
exit
```

`lessThanfFailure.input`
```bash=
python proxy.py
0 start 4 10013
1 start 4 10014
2 start 4 10015
3 start 4 10016
0 msg 0 WhatsYourName
-1 waitForAck 0
3 crash
1 msg 1 Alice
2 msg 2 Bob
-1 waitForAck 1
-1 waitForAck 2
0 get chatLog
WhatsYourName,Alice,Bob
1 get chatLog
WhatsYourName,Alice,Bob
2 get chatLog
WhatsYourName,Alice,Bob
exit
```

`recover.input`
```bash=
sophiewu7@Pro7:~/network/a4$ python proxy.py
0 start 4 10029
1 start 4 10030
2 start 4 10031
3 start 4 10032
0 msg 0 WhatsYourName
-1 waitForAck 0
3 crash
1 msg 1 Alice
2 msg 2 Bob
-1 waitForAck 1
-1 waitForAck 2
3 start 4 10004
3 msg 3 Carol
-1 waitForAck 3
0 get chatLog
WhatsYourName,Alice,Bob,Carol
3 get chatLog
WhatsYourName,Alice,Bob,Carol
exit
```

### Other Testcases

Only message with consecutive msgID can be logged.
```bash=
sophiewu7@Pro7:~/network/a4$ python proxy.py
0 start 4 10000
1 start 4 10001
2 start 4 10002
3 start 4 10003
0 msg 0 WhatsYourName
1 msg 1 Alice
2 msg 2 Bob
3 msg 3 Carol
0 msg 5 MyNameIsSophie
0 get chatLog
WhatsYourName,Alice,Bob,Carol
0 msg 4 HEYYY
1 msg 4 Hi
0 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie
1 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie
2 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie
3 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie
0 msg 6 Sorry
0 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie,Sorry
1 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie,Sorry
2 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie,Sorry
3 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie,Sorry
3 msg 7 WhatHappaned
0 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie,Sorry,WhatHappaned
1 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie,Sorry,WhatHappaned
2 get chatLog
WhatsYourName,Alice,Bob,Carol,Hi,MyNameIsSophie,Sorry,WhatHappaned
exit
```

crash two servers and recover:
```bash=
sophiewu7@Pro7:~/network/a4$ python proxy.py
0 start 4 10000
1 start 4 10001
2 start 4 10002
3 start 4 10003
0 msg 0 WhatsYourName
0 msg 1 Alice
0 get chatLog
WhatsYourName,Alice
1 msg 2 Bob
1 get chatLog
WhatsYourName,Alice,Bob
2 get chatLog
WhatsYourName,Alice,Bob
0 crash
0 start 4 10004
0 get chatLog
<Empty>
0 get chatLog
<Empty>
0 get chatLog
<Empty>
0 get chatLog
WhatsYourName,Alice,Bob
0 msg 3 Carol
0 get chatLog
WhatsYourName,Alice,Bob,Carol
1 get chatLog
WhatsYourName,Alice,Bob,Carol
1 msg 4 David
1 get chatLog
WhatsYourName,Alice,Bob,Carol,David
1 crash
2 msg 5 Emily
2 get chatLog
WhatsYourName,Alice,Bob,Carol,David,Emily
0 get chatLog
WhatsYourName,Alice,Bob,Carol,David,Emily
0 crash
2 msg 6 Fred
2 get chatLog
WhatsYourName,Alice,Bob,Carol,David,Emily,Fred
3 get chatLog
WhatsYourName,Alice,Bob,Carol,David,Emily,Fred
0 start 4 10005
0 get chatLog
WhatsYourName,Alice,Bob,Carol,David,Emily,Fred
0 msg 7 Grey
0 get chatLog
WhatsYourName,Alice,Bob,Carol,David,Emily,Fred,Grey
exit
```

crash all server once and recover
```bash=
sophiewu7@Pro7:~/network/a4$ python proxy.py
0 start 5 10100
1 start 5 10101
2 start 5 10102
3 start 5 10103
4 start 5 10104
0 msg 0 WhatsYourName
1 msg 1 Alice
2 msg 2 Bob
3 msg 3 Cindy
4 msg 4 David
0 crash
1 msg 5 Hello
0 start 5 10105
0 get chatLog
<Empty>
0 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello
2 msg 6 Hey
2 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello,Hey
1 crash
3 msg 7 Hi
0 msg 8 IamEffie
1 start 5 10106
2 crash
3 crash
4 msg 9 HiEffie
4 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello,Hey,Hi,IamEffie,HiEffie
2 start 5 10107
3 start 5 10108
4 crash
0 crash
1 msg 10 Bye
1 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello,Hey,Hi,IamEffie,HiEffie,Bye
2 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello,Hey,Hi,IamEffie,HiEffie,Bye
0 start 5 10109
4 start 5 10110
0 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello,Hey,Hi,IamEffie,HiEffie,Bye
4 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello,Hey,Hi,IamEffie,HiEffie,Bye
0 msg 11 AnybodyHere
4 get chatLog
WhatsYourName,Alice,Bob,Cindy,David,Hello,Hey,Hi,IamEffie,HiEffie,Bye,AnybodyHere
exit
```
## Limitations and Future Work

We will not talk about the inherent limitation of Raft system itself. Instead, we will focus specifically on the limitations introduced by our particular implementation. 

### Limitation 1: Consecutive Message IDs

#### Description
In the current implementation, the system rely on `<messageID>` provided by client through proxy to identify the order. Therefore, currently the system assumes that message IDs are strictly consecutive. If message IDs are not sequential due to a client error or network issues causing message loss or reordering, the RAFT protocol's ability to maintain a correct and consistent order of log entries might be compromised.
    * For example: If message 1-3 are already commited, then 
```plaintext=
1 msg 5 hello
1 msg 4 sophie
2 msg 4 lexi
```
Then `get chatLog`,  the output will be: 
```plaintext=
<message1>,<message2>,<message3>,lexi,hello
```
The `1 msg 4 sophie` is lost. 

If we do not input `2 msg 4 lexi`, then the two message (`hello,sophie`) sent to server 1 will be stuck in the `MessageQueue` and will not be written into the log. 

#### Future Work
We think use timestamp or order server id mechanism will be helpful, but in the context of this lab (we are required to input message IDs through proxy), we didn't implement it. Also, we need to make our Message Queue management strategy more robost so that it can process non-consecutive message ID. 

### Limitation 2: No Persistent Storage

#### Description

Due to uncertainties about the auto-grader's ability to clear log files (this lab handout does not ask us to write the log into files but lab3 does so), we choose not to write logs to persistent storage.

To minimize the possibility of losing message, we will not allow the follower to pop out message when the message has not been committed by the majority of servers. In this way, even the leader crash before the message is committed, the message will not lost. However, if the follower crash before it send out the message, the message will lost. 

#### Future Work

For further reduce the possiblity of losing message, we can implement persistent log storage. We can introduce log writing to disk. Even if a node crashes, it can recover and continue processing from the last known state.


## Reference (including the figure in this report)

University of Cambridge. (2021). Distributed systems [Lecture notes]. Computer Science Tripos, Part IB, Michaelmas term 2021/22. Retrieved from https://www.cl.cam.ac.uk/teaching/2122/ConcDisSys/dist-sys-notes.pdf

