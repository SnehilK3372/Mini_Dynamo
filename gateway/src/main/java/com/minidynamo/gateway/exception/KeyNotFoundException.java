package com.minidynamo.gateway.exception;

/** No live value for the key (absent, or a converged tombstone). Maps to HTTP 404. */
public class KeyNotFoundException extends RuntimeException {
    public KeyNotFoundException(String key) {
        super("key not found: " + key);
    }
}
