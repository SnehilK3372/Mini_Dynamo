package com.minidynamo.gateway.config;

import java.util.List;
import org.springframework.boot.context.properties.ConfigurationProperties;

/**
 * Where the cluster is and how patiently to talk to it. {@code nodes} is the seed
 * list of {@code host:port} endpoints used before the ring is discovered and as
 * the routing fallback. Once {@code RingPoller} has fetched the live ring, the
 * client routes ring-aware (straight to a key's primary owner); until then it
 * tries {@code nodes} in order, failing over on a connection error.
 */
@ConfigurationProperties(prefix = "cluster")
public class ClusterProperties {

    private List<String> nodes = List.of("localhost:5001");
    private int connectTimeoutMs = 1000;
    private int readTimeoutMs = 2000;
    // Ring-aware routing (Tier 4.4):
    private int virtualNodes = 128;            // MUST match the C++ node VNODES default
    private long ringPollIntervalMs = 5000;    // how often RingPoller refreshes the ring
    private int maxConnectionsPerNode = 4;     // per-node socket pool cap

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

    public int getVirtualNodes() {
        return virtualNodes;
    }

    public void setVirtualNodes(int virtualNodes) {
        this.virtualNodes = virtualNodes;
    }

    public long getRingPollIntervalMs() {
        return ringPollIntervalMs;
    }

    public void setRingPollIntervalMs(long ringPollIntervalMs) {
        this.ringPollIntervalMs = ringPollIntervalMs;
    }

    public int getMaxConnectionsPerNode() {
        return maxConnectionsPerNode;
    }

    public void setMaxConnectionsPerNode(int maxConnectionsPerNode) {
        this.maxConnectionsPerNode = maxConnectionsPerNode;
    }
}
