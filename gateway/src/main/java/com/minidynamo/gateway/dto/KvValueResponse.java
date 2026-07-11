package com.minidynamo.gateway.dto;

/** A single resolved value and the vector-clock token that versions it. */
public record KvValueResponse(String value, String clock) {
}
