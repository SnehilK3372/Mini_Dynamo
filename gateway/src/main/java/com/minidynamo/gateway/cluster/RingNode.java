package com.minidynamo.gateway.cluster;

/** One physical node in the cluster's consistent-hash ring. */
public record RingNode(String id, String host, int port) {
}
