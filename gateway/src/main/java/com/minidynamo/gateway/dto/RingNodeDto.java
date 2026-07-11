package com.minidynamo.gateway.dto;

/** A node as exposed by {@code GET /v1/cluster/ring} (live snapshot from the cluster). */
public record RingNodeDto(String id, String host, int port) {
}
