package com.minidynamo.gateway.dto;

import jakarta.validation.constraints.NotNull;

/**
 * Body of a {@code PUT /v1/kv/{key}}. {@code value} is the (UTF-8) payload to
 * store; {@code clock} is the vector-clock token from a prior read, echoed back
 * as causal context so this write descends from what the client last saw (null
 * or empty for a blind write).
 */
public record KvWriteRequest(
        @NotNull(message = "value is required") String value,
        String clock) {
}
