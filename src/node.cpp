
#include "node.h"
#include "router.h"
#include"message.h"
#include "net/tcp_client.h"
#include "util.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <set>

using namespace std;


static string getenv_or(const char* k, const string &def=""){
    const char* v=getenv(k);
    if (!v) return def;
    return string(v);
}


//]\sends payload to host port
static string tcp_send_recv(const string &host, uint16_t port,const string &payload, int timeout_ms=2000,bool use_length_prefix=true){
    addrinfo hints{};
    addrinfo *res=nullptr;
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;
    int rv=getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &res);
    if (rv != 0 || !res){
        ostringstream oss; oss<<"getaddrinfo failed for "<<host<<":"<<port<<" ("<<gai_strerror(rv)<<")";
        throw runtime_error(oss.str());
    }

    int sock=-1;
    for (addrinfo* p=res; p != nullptr; p=p->ai_next){
        sock=socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        struct timeval tv;
        tv.tv_sec=timeout_ms / 1000;
        tv.tv_usec=(timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock);
        sock=-1;
    }
    freeaddrinfo(res);
    if (sock < 0){
        ostringstream oss; oss<<"connect failed to "<<host<<":"<<port;
        throw runtime_error(oss.str());
    }
    if (use_length_prefix){
        uint32_t len=(uint32_t)payload.size();
        uint32_t nlen=htonl(len);
        ssize_t n=send(sock, &nlen, sizeof(nlen), MSG_NOSIGNAL);
        if (n != (ssize_t)sizeof(nlen)){ close(sock); throw runtime_error("send len failed"); }
    }

    ssize_t sent=0;
    while (sent < (ssize_t)payload.size()){
        ssize_t s=send(sock, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (s <= 0){ close(sock); throw runtime_error("send payload failed"); }
        sent += s;
    }

    string resp;
    if (use_length_prefix){
        uint32_t rlen_net;
        ssize_t r=recv(sock, &rlen_net, sizeof(rlen_net), MSG_WAITALL);
        if (r <= 0){ close(sock); return string(); }
        uint32_t rlen=ntohl(rlen_net);
        if (rlen == 0){ close(sock); return string(); }
        resp.resize(rlen);
        size_t got=0;
        while (got < rlen){
            ssize_t rr=recv(sock, &resp[got], rlen - got, 0);
            if (rr <= 0){ close(sock); throw runtime_error("recv payload failed"); }
            got += rr;
        }
    } else{
        char buf[4096];
        ssize_t n=read(sock, buf, sizeof(buf)-1);
        if (n > 0){
            buf[n]='\0';
            resp.assign(buf, n);
        }
    }
    close(sock);
    return resp;
}

static bool read_length_prefixed(int client_sock, string &out_payload){//reads length-prefixed message from blocking socket (caller passes accepted client socket)

    uint32_t nlen_net;
    ssize_t r=recv(client_sock, &nlen_net, sizeof(nlen_net), MSG_WAITALL);
    if (r <= 0) return false;
    uint32_t nlen=ntohl(nlen_net);
    out_payload.clear();
    out_payload.resize(nlen);
    size_t got=0;
    while (got < nlen){
        ssize_t rr=recv(client_sock, &out_payload[got], nlen - got, 0);
        if (rr <= 0) return false;
        got += rr;
    }
    return true;
}


static vector<string> split_lines(const string &s){
    vector<string> out;
    istringstream iss(s);
    string token;
    while (getline(iss, token, '|')){
        if (!token.empty() && token.back() == '\r') token.pop_back();
        out.push_back(token);
    }
    return out;
}

Node::Node(const NodeInfo &info_, Router *router_) : info(info_), router(router_), stop_flag(false){}
Node::~Node(){ stop(); }


//start TCP server
void Node::start(){
    uint16_t port=info.port;
    thread([this, port](){ this->server_loop(port); }).detach();

    string seed_host=getenv_or("SEED_HOST", "");
    string seed_port_s=getenv_or("SEED_PORT", "");
    if (!seed_host.empty() && !seed_port_s.empty()){
        try{
            uint16_t seed_port=(uint16_t)stoi(seed_port_s);

            //send JOIN: TYPE|NODE_ID|HOST|PORT|ORIGIN
            ostringstream oss;
            oss<<"JOIN|" 
               <<this->info.node_id<<"|" 
               <<this->info.host<<"|" 
               <<this->info.port<<"|" 
               <<this->info.node_id;//origin can be self node_id

            string resp=tcp_send_recv(seed_host, seed_port, oss.str(), 3000);

            if(!resp.empty()){
                auto lines=split_lines(resp);
                if (!lines.empty() && lines[0] == "RING_UPDATE"){
                    if (lines.size() >= 2){
                        int n=stoi(lines[1]);
                        for (int i=0; i < n && 2 + i < (int)lines.size(); ++i){
                            string nid=lines[2 + i];
                            NodeInfo ni{nid, nid, this->info.port};
                            router->addPhysicalNode(ni);
                            cout<<"["<<this->info.node_id<<"] learned node "<<nid<<""<<endl;
                        }
                //also add self to router
                        router->addPhysicalNode(this->info);
                    }
                }
            } 
            else{
                //seed returned nothing; still add self
                router->addPhysicalNode(this->info);
            }
        } 
        catch(const exception &e){
            cerr<< "[" <<info.node_id<< "] JOIN failed: "<<e.what()<<"";
            router->addPhysicalNode(this->info);
        }
    } 
    else{
        router->addPhysicalNode(this->info);
    }
}

void Node::stop(){
    stop_flag.store(true);
}
void Node::server_loop(uint16_t port){
   
}

void Node::sendMessage(const NodeInfo &dest, const Message &m){//builds payload lines and calls tcp_send_recv to remote node (host==dest.host, port==dest.port)

    string payload=m.serialize();
    try{
        string resp=tcp_send_recv(dest.host, dest.port, payload, 3000, false);
        if (!resp.empty()){
            auto parts=split_lines(resp);
            if (!parts.empty() && parts[0] == "RESPONSE"){
                cout<<"["<<info.node_id<<"] got response: "<<(parts.size()>1 ? parts[1]:"")<<"\n";
            }
        }
    } catch (const exception &e){
        cerr<<"["<<info.node_id<<"] sendMessage to "<<dest.node_id<<" failed: "<<e.what()<<endl;
    }
}

const int REPLICATION_FACTOR = 3; 

string Node::handlePut(const string& key, const string& value){// helper for handling "PUT"
    cout<<"["<<info.node_id<<"] handlePut: Storing key "<<key<<" locally.\n";
   {
        lock_guard<mutex> lock(storage_mutex);
        local_storage[key] = value;
    }

    auto all_owners = router->findOwners(key, REPLICATION_FACTOR);
    
    for (const auto& owner : all_owners){
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

void Node::handleReplicate(const string& key, const string& value){
    cout<<"["<<info.node_id<<"] handleReplicate: Storing replicated key "<<key<<"\n";
   {
        lock_guard<mutex> lock(storage_mutex);
        local_storage[key] = value;
    }
}

optional<string> Node::handleGet(const string& key){
    lock_guard<mutex> lock(storage_mutex);
    
    auto it = local_storage.find(key);
    if (it != local_storage.end()){
        return it->second; // Return the value
    }
    
    return nullopt;
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
    for (const auto& owner : owners){
        
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
        for (const auto& n : current_ring){
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
