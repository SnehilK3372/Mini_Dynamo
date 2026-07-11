package com.minidynamo.gateway.dto;

/** Result of a successful PUT/DELETE: the new vector-clock token to carry forward. */
public record WriteResponse(String clock) {
}
