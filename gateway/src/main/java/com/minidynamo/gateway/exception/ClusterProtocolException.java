package com.minidynamo.gateway.exception;

/**
 * The cluster answered, but with an error the gateway can't turn into a normal
 * result (e.g. a forwarding failure or an unexpected status). A bad upstream
 * response → HTTP 502.
 */
public class ClusterProtocolException extends RuntimeException {
    public ClusterProtocolException(String reason) {
        super("cluster error: " + reason);
    }
}
