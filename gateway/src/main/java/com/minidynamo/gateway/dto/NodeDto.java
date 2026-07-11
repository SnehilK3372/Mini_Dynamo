package com.minidynamo.gateway.dto;

import java.time.Instant;

/** A node as exposed by {@code GET /v1/cluster/nodes} (from the Postgres registry). */
public record NodeDto(String id, String host, int port, Instant addedAt, Instant lastSeen) {
}
