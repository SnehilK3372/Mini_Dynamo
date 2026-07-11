package com.minidynamo.gateway.exception;

import com.minidynamo.gateway.cluster.ClusterUnavailableException;
import com.minidynamo.gateway.dto.ApiError;
import com.minidynamo.gateway.dto.SiblingsResponse;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.security.authentication.BadCredentialsException;
import org.springframework.web.bind.MethodArgumentNotValidException;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;

/**
 * Maps domain and framework exceptions to the HTTP contract the build plan
 * specifies: 404 for a missing key, 409 for concurrent siblings, 503 when the
 * cluster can't meet quorum or is unreachable, 400 for bad request bodies.
 */
@RestControllerAdvice
public class GlobalExceptionHandler {

    private static final Logger log = LoggerFactory.getLogger(GlobalExceptionHandler.class);

    @ExceptionHandler(KeyNotFoundException.class)
    public ResponseEntity<ApiError> notFound(KeyNotFoundException e) {
        return ResponseEntity.status(HttpStatus.NOT_FOUND)
                .body(new ApiError("not_found", e.getMessage()));
    }

    @ExceptionHandler(SiblingsConflictException.class)
    public ResponseEntity<SiblingsResponse> siblings(SiblingsConflictException e) {
        // 409 Conflict (chosen over 300 Multiple Choices): siblings are a write
        // conflict to resolve, and 409 is universally understood by HTTP clients.
        return ResponseEntity.status(HttpStatus.CONFLICT)
                .body(new SiblingsResponse(e.getSiblings()));
    }

    @ExceptionHandler(QuorumNotMetException.class)
    public ResponseEntity<ApiError> quorum(QuorumNotMetException e) {
        return ResponseEntity.status(HttpStatus.SERVICE_UNAVAILABLE)
                .body(new ApiError("quorum_not_met", e.getMessage()));
    }

    @ExceptionHandler(ClusterUnavailableException.class)
    public ResponseEntity<ApiError> unavailable(ClusterUnavailableException e) {
        log.warn("cluster unavailable", e);
        return ResponseEntity.status(HttpStatus.SERVICE_UNAVAILABLE)
                .body(new ApiError("cluster_unavailable", "no cluster node could be reached"));
    }

    @ExceptionHandler(ClusterProtocolException.class)
    public ResponseEntity<ApiError> protocol(ClusterProtocolException e) {
        log.warn("cluster protocol error", e);
        return ResponseEntity.status(HttpStatus.BAD_GATEWAY)
                .body(new ApiError("cluster_error", e.getMessage()));
    }

    @ExceptionHandler(BadCredentialsException.class)
    public ResponseEntity<ApiError> badCredentials(BadCredentialsException e) {
        return ResponseEntity.status(HttpStatus.UNAUTHORIZED)
                .body(new ApiError("invalid_credentials", e.getMessage()));
    }

    @ExceptionHandler(MethodArgumentNotValidException.class)
    public ResponseEntity<ApiError> validation(MethodArgumentNotValidException e) {
        String detail = e.getBindingResult().getFieldErrors().stream()
                .findFirst()
                .map(f -> f.getField() + ": " + f.getDefaultMessage())
                .orElse("invalid request");
        return ResponseEntity.badRequest().body(new ApiError("bad_request", detail));
    }

    @ExceptionHandler(IllegalArgumentException.class)
    public ResponseEntity<ApiError> illegalArg(IllegalArgumentException e) {
        return ResponseEntity.badRequest().body(new ApiError("bad_request", e.getMessage()));
    }
}
