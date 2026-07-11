package com.minidynamo.gateway.config;

import org.springframework.boot.context.properties.ConfigurationProperties;

/**
 * The single demo credential the token endpoint accepts. A real system would
 * authenticate against a user store; for this project one env-configured
 * account ({@code AUTH_USERNAME}/{@code AUTH_PASSWORD}) is enough to demonstrate
 * the JWT issue/validate flow without pulling in a user database.
 */
@ConfigurationProperties(prefix = "auth")
public class AuthProperties {

    private String username = "admin";
    private String password = "changeme";

    public String getUsername() {
        return username;
    }

    public void setUsername(String username) {
        this.username = username;
    }

    public String getPassword() {
        return password;
    }

    public void setPassword(String password) {
        this.password = password;
    }
}
