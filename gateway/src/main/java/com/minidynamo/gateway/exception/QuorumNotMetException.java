package com.minidynamo.gateway.exception;

/**
 * The cluster was reachable but could not assemble the requested W (write) or R
 * (read) quorum in time — a retryable condition. Maps to HTTP 503.
 */
public class QuorumNotMetException extends RuntimeException {
    public QuorumNotMetException() {
        super("cluster could not meet the requested quorum");
    }
}
