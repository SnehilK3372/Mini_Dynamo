package com.minidynamo.gateway.service;

import com.minidynamo.gateway.cluster.ClusterClient;
import com.minidynamo.gateway.cluster.ReadResult;
import com.minidynamo.gateway.cluster.ValueVersion;
import com.minidynamo.gateway.cluster.WriteResult;
import com.minidynamo.gateway.dto.KvValueResponse;
import com.minidynamo.gateway.dto.KvWriteRequest;
import com.minidynamo.gateway.dto.WriteResponse;
import com.minidynamo.gateway.entity.AuditLogEntity;
import com.minidynamo.gateway.exception.ClusterProtocolException;
import com.minidynamo.gateway.exception.KeyNotFoundException;
import com.minidynamo.gateway.exception.QuorumNotMetException;
import com.minidynamo.gateway.exception.SiblingsConflictException;
import com.minidynamo.gateway.repository.AuditLogRepository;
import com.minidynamo.gateway.repository.ConfigVersionRepository;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

/**
 * The key-value use cases: translate a validated REST request into a cluster
 * operation, resolve N/W/R (request override, else the current config version's
 * defaults), and map the cluster's answer onto the gateway's HTTP contract.
 * Deletes are audited.
 */
@Service
public class KvService {

    private static final Logger log = LoggerFactory.getLogger(KvService.class);

    private final ClusterClient cluster;
    private final ConfigVersionRepository configRepo;
    private final AuditLogRepository auditRepo;

    public KvService(ClusterClient cluster, ConfigVersionRepository configRepo,
                     AuditLogRepository auditRepo) {
        this.cluster = cluster;
        this.configRepo = configRepo;
        this.auditRepo = auditRepo;
    }

    /** N/W/R currently in effect: the latest config version, or Dynamo's 3/2/2. */
    private record Quorum(int n, int w, int r) {
    }

    private Quorum defaults() {
        return configRepo.findFirstByOrderByVersionDesc()
                .map(c -> new Quorum(c.getN(), c.getW(), c.getR()))
                .orElse(new Quorum(3, 2, 2));
    }

    private static int orDefault(Integer requested, int fallback) {
        return (requested != null && requested > 0) ? requested : fallback;
    }

    public WriteResponse put(String key, KvWriteRequest body, Integer n, Integer w, Integer r) {
        Quorum d = defaults();
        byte[] value = body.value().getBytes(StandardCharsets.UTF_8);
        WriteResult res = cluster.put(key, value, body.clock(),
                orDefault(n, d.n()), orDefault(w, d.w()), orDefault(r, d.r()));
        return switch (res.status()) {
            case OK -> new WriteResponse(res.clock());
            case QUORUM_FAILED -> throw new QuorumNotMetException();
            case ERROR -> throw new ClusterProtocolException(res.error());
        };
    }

    public KvValueResponse get(String key, Integer n, Integer r) {
        Quorum d = defaults();
        ReadResult res = cluster.get(key, orDefault(n, d.n()), orDefault(r, d.r()));
        return switch (res.status()) {
            case OK -> toDto(res.values().get(0));
            case SIBLINGS -> throw new SiblingsConflictException(res.values().stream()
                    .map(KvService::toDto).toList());
            case NOT_FOUND -> throw new KeyNotFoundException(key);
            case QUORUM_FAILED -> throw new QuorumNotMetException();
            case ERROR -> throw new ClusterProtocolException(res.error());
        };
    }

    public WriteResponse delete(String key, String clock, Integer n, Integer w, String actor) {
        Quorum d = defaults();
        int en = orDefault(n, d.n());
        int ew = orDefault(w, d.w());
        String before = captureBefore(key, en, d.r());

        WriteResult res = cluster.delete(key, clock, en, ew);
        return switch (res.status()) {
            case OK -> {
                audit(actor, key, before);
                yield new WriteResponse(res.clock());
            }
            case QUORUM_FAILED -> throw new QuorumNotMetException();
            case ERROR -> throw new ClusterProtocolException(res.error());
        };
    }

    /** Best-effort read of the value being deleted, for the audit trail. */
    private String captureBefore(String key, int n, int r) {
        try {
            ReadResult res = cluster.get(key, n, r);
            if (res.status() == ReadResult.Status.OK) {
                return new String(res.values().get(0).value(), StandardCharsets.UTF_8);
            }
        } catch (RuntimeException e) {
            log.debug("could not capture pre-delete value for {}: {}", key, e.toString());
        }
        return null;
    }

    private void audit(String actor, String key, String before) {
        auditRepo.save(new AuditLogEntity(actor, "DELETE", key, before, null, Instant.now()));
    }

    private static KvValueResponse toDto(ValueVersion v) {
        return new KvValueResponse(new String(v.value(), StandardCharsets.UTF_8), v.clock());
    }
}
