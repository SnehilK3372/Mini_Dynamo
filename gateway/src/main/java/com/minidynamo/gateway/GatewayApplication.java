package com.minidynamo.gateway;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.ConfigurationPropertiesScan;

/**
 * Entry point for the Mini Dynamo gateway: a stateless Spring Boot service that
 * authenticates callers (JWT), validates requests, forwards key-value operations
 * to the C++ cluster over its TCP protocol, and keeps cluster metadata in
 * PostgreSQL. It owns no data itself — the cluster is the source of truth for
 * values, Postgres for administrative metadata.
 */
@SpringBootApplication
@ConfigurationPropertiesScan  // registers the @ConfigurationProperties beans (cluster/jwt/auth)
public class GatewayApplication {
    public static void main(String[] args) {
        SpringApplication.run(GatewayApplication.class, args);
    }
}
