package com.minidynamo.gateway.config;

import org.springframework.boot.context.properties.ConfigurationProperties;

/**
 * JWT signing configuration. The secret is an HMAC-SHA256 key and must be at
 * least 32 bytes; it comes from the environment ({@code JWT_SECRET}) in any real
 * deployment, never from source. Expiry is deliberately short (stateless tokens
 * can't be revoked, so they should not live long).
 */
@ConfigurationProperties(prefix = "jwt")
public class JwtProperties {

    private String secret;
    private long expiryMinutes = 30;

    public String getSecret() {
        return secret;
    }

    public void setSecret(String secret) {
        this.secret = secret;
    }

    public long getExpiryMinutes() {
        return expiryMinutes;
    }

    public void setExpiryMinutes(long expiryMinutes) {
        this.expiryMinutes = expiryMinutes;
    }
}
