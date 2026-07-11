package com.minidynamo.gateway.cluster;

/**
 * Outcome of a cluster PUT or DELETE. {@code OK} carries the new vector-clock
 * token the client should echo as context on its next write; {@code
 * QUORUM_FAILED} means fewer than W replicas acknowledged in time (retryable).
 */
public record WriteResult(Status status, String clock, String error) {

    public enum Status { OK, QUORUM_FAILED, ERROR }

    public static WriteResult ok(String clock) {
        return new WriteResult(Status.OK, clock, null);
    }

    public static WriteResult quorumFailed() {
        return new WriteResult(Status.QUORUM_FAILED, null, "quorum_not_met");
    }

    public static WriteResult error(String reason) {
        return new WriteResult(Status.ERROR, null, reason);
    }
}
