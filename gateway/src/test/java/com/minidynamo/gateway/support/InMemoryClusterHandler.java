package com.minidynamo.gateway.support;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Function;

/**
 * Minimal in-memory emulation of the cluster's request/response semantics, enough
 * for an end-to-end gateway test: PUT stores a value + clock, GET returns it,
 * DELETE tombstones it (GET then 404), RING returns a fixed node list. Clocks are
 * synthesised as {@code srv:<n>} — the gateway treats them as opaque tokens.
 */
public class InMemoryClusterHandler implements Function<String, String> {

    private record Entry(String b64value, String clock, boolean deleted) {
    }

    private final Map<String, Entry> store = new ConcurrentHashMap<>();
    private final AtomicLong counter = new AtomicLong();
    private final List<String> ringLines;

    /** @param nodes each "id|host|port" line for RING responses */
    public InMemoryClusterHandler(List<String> nodes) {
        this.ringLines = nodes;
    }

    @Override
    public String apply(String request) {
        String[] f = request.split("\\|", -1);
        String type = f[0];
        return switch (type) {
            case "PUT" -> put(f);
            case "GET" -> get(f);
            case "DELETE" -> delete(f);
            case "RING" -> ring();
            default -> "RESPONSE|ERROR|unknown_command";
        };
    }

    private String put(String[] f) {
        // PUT|key|b64|origin|N|W|R|clock
        String key = f[1];
        String b64 = f[2];
        String clock = "srv:" + counter.incrementAndGet();
        store.put(key, new Entry(b64, clock, false));
        return "RESPONSE|OK|" + clock;
    }

    private String get(String[] f) {
        // GET|key|origin|N|R
        Entry e = store.get(f[1]);
        if (e == null || e.deleted()) {
            return "RESPONSE|NOTFOUND";
        }
        return "RESPONSE|OK|" + e.b64value() + "|" + e.clock();
    }

    private String delete(String[] f) {
        // DELETE|key|origin|N|W|clock
        String key = f[1];
        String clock = "srv:" + counter.incrementAndGet();
        store.put(key, new Entry("", clock, true));
        return "RESPONSE|OK|" + clock;
    }

    private String ring() {
        StringBuilder sb = new StringBuilder("RING\n").append(ringLines.size()).append("\n");
        for (String line : ringLines) {
            sb.append(line).append("\n");
        }
        return sb.toString();
    }
}
