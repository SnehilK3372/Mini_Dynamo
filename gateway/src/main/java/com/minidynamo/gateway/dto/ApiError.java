package com.minidynamo.gateway.dto;

/** Uniform error body: a short machine code and a human-readable message. */
public record ApiError(String error, String message) {
}
