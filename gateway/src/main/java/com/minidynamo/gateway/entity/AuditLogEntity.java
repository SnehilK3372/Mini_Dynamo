package com.minidynamo.gateway.entity;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.GeneratedValue;
import jakarta.persistence.GenerationType;
import jakarta.persistence.Id;
import jakarta.persistence.Table;
import java.time.Instant;

/** One administrative action recorded in the {@code audit_log} table. */
@Entity
@Table(name = "audit_log")
public class AuditLogEntity {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long id;

    @Column(nullable = false)
    private String actor;

    @Column(nullable = false, length = 64)
    private String action;

    @Column(name = "target_key", length = 512)
    private String targetKey;

    @Column(name = "before", columnDefinition = "text")
    private String before;

    @Column(name = "after", columnDefinition = "text")
    private String after;

    @Column(name = "at", nullable = false)
    private Instant at;

    protected AuditLogEntity() {
        // JPA
    }

    public AuditLogEntity(String actor, String action, String targetKey, String before,
                          String after, Instant at) {
        this.actor = actor;
        this.action = action;
        this.targetKey = targetKey;
        this.before = before;
        this.after = after;
        this.at = at;
    }

    public Long getId() {
        return id;
    }

    public String getActor() {
        return actor;
    }

    public String getAction() {
        return action;
    }

    public String getTargetKey() {
        return targetKey;
    }

    public String getBefore() {
        return before;
    }

    public String getAfter() {
        return after;
    }

    public Instant getAt() {
        return at;
    }
}
