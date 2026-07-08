#include "node.h"
#include "router.h"
#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>
#include <vector>
#include "net/tcp_server.h"
#include "net/tcp_client.h"
#include "message.h"

using namespace std;


string getenv_str(const string &var, const string &default_val=""){
    const char* val = getenv(var.c_str());
    return val ? string(val) : default_val;
}


static vector<string> split_lines_by_newline(const string &s){
    vector<string> out;
    istringstream iss(s);
    string line;
    while (getline(iss, line, '\n')){ // Split by newline
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()){
            out.push_back(line);
        }
    }
    return out;
}

static vector<string> split_string(const string &s, char delim){//msg splitter
    vector<string> out;
    istringstream iss(s);
    string token;
    while (getline(iss, token, delim)){
        out.push_back(token);
    }
    return out;
}





//Instantiates the Router and Node objects
//inserts virtual nodes into the hash ring
//boots the node with start()
int main(){
    cout.setf(ios::unitbuf);

    //loading env variables
    string node_id = getenv_str("NODE_ID");
    string host = getenv_str("HOST", "0.0.0.0");
    uint16_t port = stoi(getenv_str("NODE_PORT"));

    string bootstrap_ip = getenv_str("BOOTSTRAP_IP", "");
    uint16_t bootstrap_port = (uint16_t)stoi(getenv_str("BOOTSTRAP_PORT", "0"));

    bool isBootstrap = bootstrap_ip.empty();

    Router router;
    NodeInfo myInfo(node_id, node_id, port);
    Node node(myInfo, &router);

    router.addPhysicalNode(myInfo);//prevents race condtion

    TCPServer server(host, port, &node);
    thread serverThread([&server](){
        server.start();
    });
    serverThread.detach();

    cout << "[" << node_id << "] TCP Server started on " << host << ":" << port << "\n";

    if (isBootstrap){//logic for bootstrapping node
        //router.addPhysicalNode(myInfo);
        cout << "[" << node_id << "] Started as BOOTSTRAP node.\n";

    } else{
        // Join existing cluster
        cout << "[" << node_id << "] contacting bootstrap at " 
             << bootstrap_ip << ":" << bootstrap_port << "\n";

        TCPClient client;

        Message joinMsg;
        joinMsg.type = "JOIN";
        joinMsg.origin = node_id;
        joinMsg.key = node_id;
        joinMsg.host = node_id;
        joinMsg.port = myInfo.port;
        joinMsg.value = to_string(myInfo.port);

        string resp = client.sendAndReceive(bootstrap_ip, bootstrap_port, joinMsg.serialize());

        if (!resp.empty()){
            cout << "[" << node_id << "] JOIN response received.\n";
                        auto lines = split_lines_by_newline(resp);
            
            if (!lines.empty() && lines[0] == "RING_UPDATE"){
                if (lines.size() >= 2){
                    try{
                        int n = stoi(lines[1]);
                        cout << "[" << node_id << "] Learning " << n << " existing nodes...\n";
                        
                        for (int i = 0; i < n && 2 + i < (int)lines.size(); ++i){
                            // Server sends: NODE_ID|HOST|PORT
                            auto parts = split_string(lines[2 + i], '|');
                            
                            if (parts.size() == 3){
                                string nid = parts[0];
                                string nhost = parts[1];
                                uint16_t nport = (uint16_t)stoi(parts[2]);
                                
                                NodeInfo ni{nid, nhost, nport};
                                router.addPhysicalNode(ni);
                                cout << "[" << node_id << "] ... learned node " << nid << "\n";
                            }
                        }
                    } catch (const exception& e){
                        cerr << "[" << node_id << "] Failed to parse RING_UPDATE: " << e.what() << "\n";
                    }
                }
            } else{
                cerr << "[" << node_id << "] Received malformed JOIN response: " << (lines.empty() ? resp : lines[0]) << "\n";
            }
            
        } else{
            cerr << "[" << node_id << "] JOIN request FAILED (no response)." << endl;
        }
    }
    while(true){
        this_thread::sleep_for(chrono::seconds(20));
    }

    return 0;
}