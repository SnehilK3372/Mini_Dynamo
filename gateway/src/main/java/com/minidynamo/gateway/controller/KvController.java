package com.minidynamo.gateway.controller;

import com.minidynamo.gateway.dto.KvValueResponse;
import com.minidynamo.gateway.dto.KvWriteRequest;
import com.minidynamo.gateway.dto.WriteResponse;
import com.minidynamo.gateway.service.KvService;
import jakarta.validation.Valid;
import java.security.Principal;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

/**
 * The key-value data plane. Thin by design: validate the key, delegate to
 * {@link KvService}, let the {@code GlobalExceptionHandler} map domain outcomes
 * (missing key &rarr; 404, siblings &rarr; 409, quorum failure &rarr; 503) to HTTP.
 * N/W/R are optional query overrides of the cluster defaults.
 */
@RestController
@RequestMapping("/v1/kv")
public class KvController {

    private final KvService kv;

    public KvController(KvService kv) {
        this.kv = kv;
    }

    @PutMapping("/{key}")
    public WriteResponse put(@PathVariable String key,
                             @Valid @RequestBody KvWriteRequest body,
                             @RequestParam(name = "N", required = false) Integer n,
                             @RequestParam(name = "W", required = false) Integer w,
                             @RequestParam(name = "R", required = false) Integer r) {
        validateKey(key);
        return kv.put(key, body, n, w, r);
    }

    @GetMapping("/{key}")
    public KvValueResponse get(@PathVariable String key,
                               @RequestParam(name = "N", required = false) Integer n,
                               @RequestParam(name = "R", required = false) Integer r) {
        validateKey(key);
        return kv.get(key, n, r);
    }

    @DeleteMapping("/{key}")
    public WriteResponse delete(@PathVariable String key,
                                @RequestParam(name = "clock", required = false) String clock,
                                @RequestParam(name = "N", required = false) Integer n,
                                @RequestParam(name = "W", required = false) Integer w,
                                Principal principal) {
        validateKey(key);
        String actor = principal != null ? principal.getName() : "anonymous";
        return kv.delete(key, clock, n, w, actor);
    }

    /**
     * The wire protocol is pipe-delimited with no escaping, so a '|' in a key
     * would corrupt framing. Reject it at the edge (400) rather than let it reach
     * the cluster.
     */
    private static void validateKey(String key) {
        if (key == null || key.isEmpty()) {
            throw new IllegalArgumentException("key must not be empty");
        }
        if (key.indexOf('|') >= 0) {
            throw new IllegalArgumentException("key must not contain '|'");
        }
    }
}
