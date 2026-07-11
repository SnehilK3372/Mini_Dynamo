package com.minidynamo.gateway.dto;

import jakarta.validation.constraints.NotBlank;

/** Credentials presented to {@code POST /v1/auth/token} to obtain a JWT. */
public record TokenRequest(
        @NotBlank(message = "username is required") String username,
        @NotBlank(message = "password is required") String password) {
}
