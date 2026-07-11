package com.minidynamo.gateway.entity;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.GeneratedValue;
import jakarta.persistence.GenerationType;
import jakarta.persistence.Id;
import jakarta.persistence.Table;
import java.time.Instant;

/** One row in the append-only {@code config_versions} history of N/W/R defaults. */
@Entity
@Table(name = "config_versions")
public class ConfigVersionEntity {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long version;

    @Column(nullable = false)
    private int n;

    @Column(nullable = false)
    private int w;

    @Column(nullable = false)
    private int r;

    @Column(name = "created_at", nullable = false)
    private Instant createdAt;

    protected ConfigVersionEntity() {
        // JPA
    }

    public ConfigVersionEntity(int n, int w, int r, Instant createdAt) {
        this.n = n;
        this.w = w;
        this.r = r;
        this.createdAt = createdAt;
    }

    public Long getVersion() {
        return version;
    }

    public int getN() {
        return n;
    }

    public int getW() {
        return w;
    }

    public int getR() {
        return r;
    }

    public Instant getCreatedAt() {
        return createdAt;
    }
}
