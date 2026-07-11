package com.minidynamo.gateway.cluster;

import java.util.List;

/**
 * Outcome of a cluster GET. {@code OK} carries exactly one version; {@code
 * SIBLINGS} carries the concurrent versions the client must reconcile; {@code
 * NOT_FOUND} means no live value (including a converged tombstone); {@code
 * QUORUM_FAILED} means fewer than R replicas answered in time.
 */
public record ReadResult(Status status, List<ValueVersion> values, String error) {

    public enum Status { OK, SIBLINGS, NOT_FOUND, QUORUM_FAILED, ERROR }

    public static ReadResult ok(ValueVersion v) {
        return new ReadResult(Status.OK, List.of(v), null);
    }

    public static ReadResult siblings(List<ValueVersion> vs) {
        return new ReadResult(Status.SIBLINGS, vs, null);
    }

    public static ReadResult notFound() {
        return new ReadResult(Status.NOT_FOUND, List.of(), null);
    }

    public static ReadResult quorumFailed() {
        return new ReadResult(Status.QUORUM_FAILED, List.of(), "quorum_not_met");
    }

    public static ReadResult error(String reason) {
        return new ReadResult(Status.ERROR, List.of(), reason);
    }
}
