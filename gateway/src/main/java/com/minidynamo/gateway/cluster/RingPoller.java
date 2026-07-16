package com.minidynamo.gateway.cluster;

import com.minidynamo.gateway.service.NodeRegistryService;
import java.util.List;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Component;

/**
 * Keeps the gateway's client-side ring ({@link RingRouter}) current by polling
 * the cluster's live ring on a fixed interval. One {@code RING} fetch per tick
 * feeds both the router (for ring-aware routing) and the Postgres node registry.
 *
 * <p>Best-effort: a failed poll (cluster briefly unreachable) is logged and the
 * previous ring snapshot stays in effect. Until the first successful poll the
 * router is empty and {@link ClusterClient} falls back to its static node list,
 * so routing is never a hard dependency for serving traffic.
 */
@Component
public class RingPoller {

    private static final Logger log = LoggerFactory.getLogger(RingPoller.class);

    private final ClusterClient cluster;
    private final RingRouter ringRouter;
    private final NodeRegistryService registry;

    public RingPoller(ClusterClient cluster, RingRouter ringRouter, NodeRegistryService registry) {
        this.cluster = cluster;
        this.ringRouter = ringRouter;
        this.registry = registry;
    }

    // initialDelay 0 → poll once as soon as the scheduler starts, then every interval.
    @Scheduled(initialDelayString = "0",
               fixedDelayString = "${cluster.ring-poll-interval-ms:5000}")
    public void poll() {
        try {
            List<RingNode> ring = cluster.ring();
            if (ring.isEmpty()) {
                log.debug("ring poll returned no nodes; keeping previous snapshot");
                return;
            }
            ringRouter.rebuild(ring);
            registry.persist(ring);
        } catch (RuntimeException e) {
            // Cluster not reachable this tick — keep the last good ring.
            log.debug("ring poll failed, keeping previous snapshot: {}", e.toString());
        }
    }
}
