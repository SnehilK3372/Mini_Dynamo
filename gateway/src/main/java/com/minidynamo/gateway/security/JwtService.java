package com.minidynamo.gateway.security;

import com.minidynamo.gateway.config.JwtProperties;
import io.jsonwebtoken.Claims;
import io.jsonwebtoken.JwtException;
import io.jsonwebtoken.Jwts;
import io.jsonwebtoken.security.Keys;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.Date;
import java.util.List;
import javax.crypto.SecretKey;
import org.springframework.stereotype.Service;

/**
 * Issues and validates HMAC-SHA256 JWTs. Signing key is derived from the
 * configured secret; a token carries the subject (username) and a {@code roles}
 * claim, plus issued-at and expiry. Validation ({@link #parse}) verifies the
 * signature and expiry — a tampered or expired token throws, which the filter
 * turns into a 401.
 */
@Service
public class JwtService {

    private final SecretKey key;
    private final long expirySeconds;

    public JwtService(JwtProperties props) {
        byte[] secret = props.getSecret().getBytes(StandardCharsets.UTF_8);
        // Keys.hmacShaKeyFor enforces >= 256 bits, so a too-short secret fails
        // fast at startup rather than silently weakening the signature.
        this.key = Keys.hmacShaKeyFor(secret);
        this.expirySeconds = Duration.ofMinutes(props.getExpiryMinutes()).toSeconds();
    }

    /** Mint a signed token for {@code username} with the given roles. */
    public String issue(String username, List<String> roles) {
        Instant now = Instant.now();
        return Jwts.builder()
                .subject(username)
                .claim("roles", roles)
                .issuedAt(Date.from(now))
                .expiration(Date.from(now.plusSeconds(expirySeconds)))
                .signWith(key)
                .compact();
    }

    /**
     * Verify signature + expiry and return the claims. Throws {@link JwtException}
     * for any invalid token (bad signature, expired, malformed).
     */
    public Claims parse(String token) throws JwtException {
        return Jwts.parser()
                .verifyWith(key)
                .build()
                .parseSignedClaims(token)
                .getPayload();
    }

    public long getExpirySeconds() {
        return expirySeconds;
    }
}
