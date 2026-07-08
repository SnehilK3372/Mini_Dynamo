#include "node.h"

#include <sys/socket.h>

#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#include "message.h"
#include "net/tcp_client.h"
#include "router.h"

using namespace std;

Node::Node(const NodeInfo &info_, Router *router_, unique_ptr<StorageEngine> storage_)
    : info(info_), router(router_), storage(move(storage_)){}

// How many distinct physical nodes should hold each key. Becomes the
// per-request N once tunable quorum lands in Tier 1A.
const int REPLICATION_FACTOR = 3;

string Node::handlePut(const string &key, const string &value){// helper for handling "PUT"
    cout<<"["<<info.node_id<<"] handlePut: Storing key "<<key<<" locally.\n";
    storage->put(key, value);

    auto all_owners = router->findOwners(key, REPLICATION_FACTOR);

    // Fire-and-forget replication: we do NOT wait for replica acks, so "OK"
    // only guarantees the local write. Real W-quorum acks are Tier 1A work.
    for (const auto &owner : all_owners){
        if (owner.node_id == this->info.node_id){
            continue;
        }

        cout<<"["<<info.node_id<<"] handlePut: Replicating key "<<key
            <<" to "<<owner.node_id<<"\n";

        Message replicateMsg;
        replicateMsg.type = "REPLICATE";
        replicateMsg.key = key;
        replicateMsg.value = value;
        replicateMsg.origin = this->info.node_id;

        TCPClient client;
        client.send(owner.host, owner.port, replicateMsg.serialize());
    }

    return "OK";
}

void Node::handleReplicate(const string &key, const string &value){
    cout<<"["<<info.node_id<<"] handleReplicate: Storing replicated key "<<key<<"\n";
    storage->put(key, value);
}

optional<string> Node::handleGet(const string &key){
    return storage->get(key);
}

void Node::onMessageReceived(const Message &msg, int client_fd){

    cout<<"["<<info.node_id<<"] Received message type="<<msg.type<<"\n";

    // --- PUT HANDLER (with Coordinator Logic) ---
    if (msg.type == "PUT"){
        auto owners = router->findOwners(msg.key, 1);
        if (owners.empty()){
            string resp = "RESPONSE|ERROR|no_nodes_in_ring\n";
            send(client_fd, resp.c_str(), resp.size(), 0);
            return;
        }

        NodeInfo primaryOwner = owners[0];

        if (primaryOwner.node_id == this->info.node_id){

            cout<<"["<<info.node_id<<"] PUT key="<<msg.key<<" (local)\n";
            string res = handlePut(msg.key, msg.value);
            string resp = "RESPONSE|" + res + "\n";
            send(client_fd, resp.c_str(), resp.size(), 0);
        } else{
            cout<<"["<<info.node_id<<"] PUT key="<<msg.key
                     <<" (forwarding to "<<primaryOwner.node_id<<")\n";
            Message putMsg = msg;
            TCPClient tcpClient;
            string rawResponse = tcpClient.sendAndReceive(
                primaryOwner.host, primaryOwner.port, putMsg.serialize()
            );

            if (rawResponse.empty()){
                string resp = "RESPONSE|ERROR|forward_failed\n";
                send(client_fd, resp.c_str(), resp.size(), 0);
            } else{
                // Relay the owner's response
                send(client_fd, rawResponse.c_str(), rawResponse.size(), 0);
            }
        }
    }


else if (msg.type == "GET"){
    auto owners = router->findOwners(msg.key, REPLICATION_FACTOR);// full list of potential owners

    if (owners.empty()){
        string resp = "RESPONSE|NOTFOUND|no_nodes_in_ring\n";
        send(client_fd, resp.c_str(), resp.size(), 0);
        return;
    }

    bool found = false;
    for (const auto &owner : owners){

        if (owner.node_id == this->info.node_id){
            cout<<"["<<info.node_id<<"] GET key="<<msg.key<<" (checking local)\n";
            auto val = handleGet(msg.key);

            if (val){
                string resp = "RESPONSE|OK|" + *val + "\n";
                send(client_fd, resp.c_str(), resp.size(), 0);
                found = true;
                break;
            }

        }
        else{
            cout<<"["<<info.node_id<<"] GET key="<<msg.key
                <<" (forwarding to "<<owner.node_id<<")\n";

            Message getMsg;
            getMsg.type = "GET";
            getMsg.key = msg.key;
            getMsg.origin = this->info.node_id;

            TCPClient tcpClient;
            string rawResponse = tcpClient.sendAndReceive(
                owner.host, owner.port, getMsg.serialize()
            );
            if (!rawResponse.empty()){
                if (rawResponse.find("NOTFOUND") == string::npos){
                    send(client_fd, rawResponse.c_str(), rawResponse.size(), 0);
                    found = true;
                    break;
                }

            }
        }
    }

    if (!found){
        string resp = "RESPONSE|NOTFOUND\n";
        send(client_fd, resp.c_str(), resp.size(), 0);
    }
}
    else if (msg.type == "REPLICATE"){
        handleReplicate(msg.key, msg.value);
        string resp = "RESPONSE|OK";
        send(client_fd, resp.c_str(), resp.size(), 0);
    }
    else if (msg.type == "JOIN"){
        if (msg.key.empty() || msg.host.empty() || msg.port == 0){
            cerr<<"["<<info.node_id<<"] Received MALFORMED JOIN"
                <<" key="<<msg.key<<" host="<<msg.host<<" port="<<msg.port<<"\n";
            return;
        }

        NodeInfo ji{msg.key, msg.host, (uint16_t)msg.port};
        cout<<"["<<info.node_id<<"] JOIN from "<<ji.node_id<<" at "<<ji.host<<":"<<ji.port<<"\n";

        //gets the current ring *before* adding the new node
        vector<NodeInfo> current_ring = router->getAllPhysicalNodes();

        router->addPhysicalNode(ji);
        ostringstream oss_resp;
        oss_resp<<"RING_UPDATE\n";
        oss_resp<<current_ring.size()<<"\n";
        for (const auto &n : current_ring){
            oss_resp<<n.node_id<<"|"<<n.host<<"|"<<n.port<<"\n";
        }
        string resp = oss_resp.str();
        send(client_fd, resp.c_str(), resp.size(), 0);
    }
    // ---
    else{
        string resp = "RESPONSE|ERR_UNKNOWN_COMMAND";
        send(client_fd, resp.c_str(), resp.size(), 0);
    }
}
