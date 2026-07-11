package com.minidynamo.gateway.dto;

/** A freshly issued JWT and how long it is valid (seconds). */
public record TokenResponse(String token, String tokenType, long expiresInSeconds) {

    public static TokenResponse bearer(String token, long expiresInSeconds) {
        return new TokenResponse(token, "Bearer", expiresInSeconds);
    }
}
