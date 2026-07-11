package com.minidynamo.gateway.cluster;

/**
 * Thrown when no cluster node could be reached (all configured endpoints refused
 * the connection or timed out). Distinct from a quorum failure, which is a
 * well-formed answer from a reachable cluster. Maps to HTTP 503.
 */
public class ClusterUnavailableException extends RuntimeException {
    public ClusterUnavailableException(String message, Throwable cause) {
        super(message, cause);
    }
}
