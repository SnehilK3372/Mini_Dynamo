package com.minidynamo.gateway.controller;

import com.minidynamo.gateway.dto.TokenRequest;
import com.minidynamo.gateway.dto.TokenResponse;
import com.minidynamo.gateway.service.AuthService;
import jakarta.validation.Valid;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

/** Issues JWTs. This is the one write endpoint that is public (it's how you get a token). */
@RestController
@RequestMapping("/v1/auth")
public class AuthController {

    private final AuthService auth;

    public AuthController(AuthService auth) {
        this.auth = auth;
    }

    @PostMapping("/token")
    public TokenResponse token(@Valid @RequestBody TokenRequest request) {
        return auth.authenticate(request);
    }
}
