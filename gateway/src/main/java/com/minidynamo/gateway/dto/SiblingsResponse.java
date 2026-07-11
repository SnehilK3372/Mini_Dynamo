package com.minidynamo.gateway.dto;

import java.util.List;

/**
 * Returned (HTTP 409) when a read finds concurrent versions the client must
 * reconcile. To resolve, the client picks/merges a value and writes it back
 * using a clock that descends from all siblings' clocks.
 */
public record SiblingsResponse(List<KvValueResponse> siblings) {
}
