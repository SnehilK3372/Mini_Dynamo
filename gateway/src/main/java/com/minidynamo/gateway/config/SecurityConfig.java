package com.minidynamo.gateway.config;

import com.minidynamo.gateway.security.JwtAuthFilter;
import jakarta.servlet.http.HttpServletResponse;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.http.HttpMethod;
import org.springframework.security.config.annotation.web.builders.HttpSecurity;
import org.springframework.security.config.annotation.web.configurers.AbstractHttpConfigurer;
import org.springframework.security.config.http.SessionCreationPolicy;
import org.springframework.security.web.SecurityFilterChain;
import org.springframework.security.web.authentication.UsernamePasswordAuthenticationFilter;

/**
 * Stateless JWT security. The token endpoint and the public docs/health surface
 * are open; everything under {@code /v1/kv} and {@code /v1/cluster} requires a
 * valid token, checked by {@link JwtAuthFilter} before the controller. No
 * sessions, no CSRF token (there is no cookie/session to protect).
 */
@Configuration
public class SecurityConfig {

    private final JwtAuthFilter jwtAuthFilter;

    public SecurityConfig(JwtAuthFilter jwtAuthFilter) {
        this.jwtAuthFilter = jwtAuthFilter;
    }

    @Bean
    public SecurityFilterChain filterChain(HttpSecurity http) throws Exception {
        http
            .csrf(AbstractHttpConfigurer::disable)
            .sessionManagement(sm -> sm.sessionCreationPolicy(SessionCreationPolicy.STATELESS))
            .authorizeHttpRequests(auth -> auth
                // Public: obtain a token, read health/metrics, and browse the API docs.
                .requestMatchers(HttpMethod.POST, "/v1/auth/token").permitAll()
                .requestMatchers("/actuator/health/**", "/actuator/info",
                        "/actuator/prometheus").permitAll()
                .requestMatchers("/swagger-ui/**", "/swagger-ui.html",
                        "/v3/api-docs/**").permitAll()
                // Everything else — the data plane and admin — needs a valid token.
                .requestMatchers("/v1/kv/**", "/v1/cluster/**").authenticated()
                .anyRequest().authenticated())
            // 401 (not 403) when an unauthenticated caller hits a protected route.
            .exceptionHandling(ex -> ex.authenticationEntryPoint(
                (request, response, authEx) ->
                    response.sendError(HttpServletResponse.SC_UNAUTHORIZED, "unauthorized")))
            .addFilterBefore(jwtAuthFilter, UsernamePasswordAuthenticationFilter.class);
        return http.build();
    }
}
