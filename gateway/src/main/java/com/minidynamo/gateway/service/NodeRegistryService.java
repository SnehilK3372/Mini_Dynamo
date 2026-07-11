package com.minidynamo.gateway.service;

import com.minidynamo.gateway.cluster.ClusterClient;
import com.minidynamo.gateway.cluster.RingNode;
import com.minidynamo.gateway.dto.NodeDto;
import com.minidynamo.gateway.entity.NodeEntity;
import com.minidynamo.gateway.repository.NodeRepository;
import java.time.Instant;
import java.util.List;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.context.event.ApplicationReadyEvent;
import org.springframework.context.event.EventListener;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

/**
 * Owns the Postgres {@code nodes} registry: serves it to
 * {@code GET /v1/cluster/nodes} and keeps it in sync with the cluster's live
 * ring. Sync is best-effort — the cluster may not be reachable at startup, and a
 * failed sync must not stop the gateway from booting.
 */
@Service
public class NodeRegistryService {

    private static final Logger log = LoggerFactory.getLogger(NodeRegistryService.class);

    private final NodeRepository repo;
    private final ClusterClient cluster;

    public NodeRegistryService(NodeRepository repo, ClusterClient cluster) {
        this.repo = repo;
        this.cluster = cluster;
    }

    public List<NodeDto> listNodes() {
        return repo.findAll().stream()
                .map(n -> new NodeDto(n.getId(), n.getHost(), n.getPort(),
                        n.getAddedAt(), n.getLastSeen()))
                .toList();
    }

    /** Refresh the registry from the cluster's live ring; returns how many nodes it saw. */
    @Transactional
    public int syncFromCluster() {
        List<RingNode> ring = cluster.ring();
        Instant now = Instant.now();
        for (RingNode rn : ring) {
            NodeEntity existing = repo.findById(rn.id()).orElse(null);
            if (existing == null) {
                repo.save(new NodeEntity(rn.id(), rn.host(), rn.port(), now, now));
            } else {
                existing.setHost(rn.host());
                existing.setPort(rn.port());
                existing.setLastSeen(now);
                repo.save(existing);
            }
        }
        return ring.size();
    }

    @EventListener(ApplicationReadyEvent.class)
    public void syncOnStartup() {
        try {
            int seen = syncFromCluster();
            log.info("node registry synced from cluster ring: {} node(s)", seen);
        } catch (RuntimeException e) {
            log.warn("startup ring sync failed (cluster not reachable yet?): {}", e.toString());
        }
    }
}
