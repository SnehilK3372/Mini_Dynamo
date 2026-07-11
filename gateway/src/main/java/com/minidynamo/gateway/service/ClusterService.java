package com.minidynamo.gateway.service;

import com.minidynamo.gateway.cluster.ClusterClient;
import com.minidynamo.gateway.dto.RingNodeDto;
import java.util.List;
import org.springframework.stereotype.Service;

/**
 * Read-only cluster introspection. {@link #ring()} is the *live* ring straight
 * from a cluster node (via the read-only RING command) — as opposed to the
 * durable node registry in Postgres, which {@link NodeRegistryService} serves.
 */
@Service
public class ClusterService {

    private final ClusterClient cluster;

    public ClusterService(ClusterClient cluster) {
        this.cluster = cluster;
    }

    public List<RingNodeDto> ring() {
        return cluster.ring().stream()
                .map(n -> new RingNodeDto(n.id(), n.host(), n.port()))
                .toList();
    }
}
