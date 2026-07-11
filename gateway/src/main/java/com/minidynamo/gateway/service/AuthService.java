package com.minidynamo.gateway.service;

import com.minidynamo.gateway.config.AuthProperties;
import com.minidynamo.gateway.dto.TokenRequest;
import com.minidynamo.gateway.dto.TokenResponse;
import com.minidynamo.gateway.security.JwtService;
import java.util.List;
import org.springframework.security.authentication.BadCredentialsException;
import org.springframework.stereotype.Service;

/**
 * Authenticates the configured demo credential and mints a JWT for it. Kept
 * deliberately thin — the interesting security work is in the token itself and
 * the validating filter, not in a user store this project doesn't need.
 */
@Service
public class AuthService {

    private final AuthProperties auth;
    private final JwtService jwt;

    public AuthService(AuthProperties auth, JwtService jwt) {
        this.auth = auth;
        this.jwt = jwt;
    }

    public TokenResponse authenticate(TokenRequest request) {
        boolean ok = auth.getUsername().equals(request.username())
                && auth.getPassword().equals(request.password());
        if (!ok) {
            throw new BadCredentialsException("invalid username or password");
        }
        String token = jwt.issue(request.username(), List.of("ADMIN"));
        return TokenResponse.bearer(token, jwt.getExpirySeconds());
    }
}
