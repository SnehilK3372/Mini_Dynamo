package com.minidynamo.gateway.web;

import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import java.io.IOException;
import java.util.UUID;
import org.slf4j.MDC;
import org.springframework.core.Ordered;
import org.springframework.core.annotation.Order;
import org.springframework.lang.NonNull;
import org.springframework.stereotype.Component;
import org.springframework.util.StringUtils;
import org.springframework.web.filter.OncePerRequestFilter;

/**
 * Assigns every request a correlation id and exposes it to the logging layer via
 * MDC, so every JSON log line emitted while handling the request carries the same
 * {@code request_id}. The id is taken from an inbound {@code X-Request-Id} header
 * when a caller (or an upstream proxy) supplies one, otherwise a fresh UUID is
 * generated; either way it is echoed back in the response header so a client can
 * correlate its call with the server logs.
 *
 * <p>Ordered at highest precedence so it wraps the Spring Security filter chain
 * (default order -100) — auth successes and failures are logged with the id too.
 * The MDC key is always cleared in a finally block so ids never leak across the
 * pooled worker thread's next request.
 */
@Component
@Order(Ordered.HIGHEST_PRECEDENCE)
public class RequestIdFilter extends OncePerRequestFilter {

    public static final String HEADER = "X-Request-Id";
    public static final String MDC_KEY = "request_id";

    @Override
    protected void doFilterInternal(@NonNull HttpServletRequest request,
                                    @NonNull HttpServletResponse response,
                                    @NonNull FilterChain chain)
            throws ServletException, IOException {
        String requestId = request.getHeader(HEADER);
        if (!StringUtils.hasText(requestId)) {
            requestId = UUID.randomUUID().toString();
        }
        MDC.put(MDC_KEY, requestId);
        response.setHeader(HEADER, requestId);
        try {
            chain.doFilter(request, response);
        } finally {
            MDC.remove(MDC_KEY);
        }
    }
}
