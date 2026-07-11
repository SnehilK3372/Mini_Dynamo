package com.minidynamo.gateway.exception;

import com.minidynamo.gateway.dto.KvValueResponse;
import java.util.List;

/**
 * A read found concurrent versions (vector clocks that neither dominate). The
 * gateway surfaces them as HTTP 409 so the client can reconcile — it never picks
 * a winner on the client's behalf.
 */
public class SiblingsConflictException extends RuntimeException {

    private final transient List<KvValueResponse> siblings;

    public SiblingsConflictException(List<KvValueResponse> siblings) {
        super("concurrent versions require reconciliation");
        this.siblings = siblings;
    }

    public List<KvValueResponse> getSiblings() {
        return siblings;
    }
}
