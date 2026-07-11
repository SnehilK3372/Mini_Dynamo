package com.minidynamo.gateway.controller;

import com.minidynamo.gateway.dto.NodeDto;
import com.minidynamo.gateway.dto.RingNodeDto;
import com.minidynamo.gateway.service.ClusterService;
import com.minidynamo.gateway.service.NodeRegistryService;
import java.util.List;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

/**
 * Cluster introspection (admin). {@code /nodes} is the durable Postgres registry;
 * {@code /ring} is the live snapshot from the cluster itself — deliberately two
 * different views (registry vs. reality) so a drift between them is visible.
 */
@RestController
@RequestMapping("/v1/cluster")
public class ClusterController {

    private final ClusterService clusterService;
    private final NodeRegistryService nodeRegistry;

    public ClusterController(ClusterService clusterService, NodeRegistryService nodeRegistry) {
        this.clusterService = clusterService;
        this.nodeRegistry = nodeRegistry;
    }

    @GetMapping("/nodes")
    public List<NodeDto> nodes() {
        return nodeRegistry.listNodes();
    }

    @GetMapping("/ring")
    public List<RingNodeDto> ring() {
        return clusterService.ring();
    }
}
