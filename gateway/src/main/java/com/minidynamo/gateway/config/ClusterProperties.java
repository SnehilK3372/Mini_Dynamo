package com.minidynamo.gateway.config;

import java.util.List;
import org.springframework.boot.context.properties.ConfigurationProperties;

/**
 * Where the cluster is and how patiently to talk to it. {@code nodes} is a list
 * of {@code host:port} endpoints; the client tries them in order and fails over
 * on a connection error (any node can coordinate a request, so the list is for
 * availability, not sharding).
 */
@ConfigurationProperties(prefix = "cluster")
public class ClusterProperties {

    private List<String> nodes = List.of("localhost:5001");
    private int connectTimeoutMs = 1000;
    private int readTimeoutMs = 2000;

    public List<String> getNodes() {
        return nodes;
    }

    public void setNodes(List<String> nodes) {
        this.nodes = nodes;
    }

    public int getConnectTimeoutMs() {
        return connectTimeoutMs;
    }

    public void setConnectTimeoutMs(int connectTimeoutMs) {
        this.connectTimeoutMs = connectTimeoutMs;
    }

    public int getReadTimeoutMs() {
        return readTimeoutMs;
    }

    public void setReadTimeoutMs(int readTimeoutMs) {
        this.readTimeoutMs = readTimeoutMs;
    }
}
