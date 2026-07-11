package com.minidynamo.gateway.entity;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.Id;
import jakarta.persistence.Table;
import java.time.Instant;

/** A cluster node as recorded in the {@code nodes} registry table. */
@Entity
@Table(name = "nodes")
public class NodeEntity {

    @Id
    private String id;

    @Column(nullable = false)
    private String host;

    @Column(nullable = false)
    private int port;

    @Column(name = "added_at", nullable = false)
    private Instant addedAt;

    @Column(name = "last_seen")
    private Instant lastSeen;

    protected NodeEntity() {
        // JPA
    }

    public NodeEntity(String id, String host, int port, Instant addedAt, Instant lastSeen) {
        this.id = id;
        this.host = host;
        this.port = port;
        this.addedAt = addedAt;
        this.lastSeen = lastSeen;
    }

    public String getId() {
        return id;
    }

    public String getHost() {
        return host;
    }

    public void setHost(String host) {
        this.host = host;
    }

    public int getPort() {
        return port;
    }

    public void setPort(int port) {
        this.port = port;
    }

    public Instant getAddedAt() {
        return addedAt;
    }

    public Instant getLastSeen() {
        return lastSeen;
    }

    public void setLastSeen(Instant lastSeen) {
        this.lastSeen = lastSeen;
    }
}
