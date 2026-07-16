package com.minidynamo.gateway.cluster;

import com.minidynamo.gateway.config.ClusterProperties;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Deque;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedDeque;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

/**
 * Speaks the C++ cluster's TCP wire protocol from Java. This is the entire
 * gateway&harr;cluster boundary: length-prefixed framing (matches {@code
 * src/net/framing.cpp}), base64-encoded values (matches {@code src/base64.cpp}),
 * pipe-delimited fields.
 *
 * <p>Ring-aware routing (Tier 4.4): for a keyed op the client asks {@link
 * RingRouter} for the key's owners and tries the primary first, then the other
 * owners, then any remaining seed node as a last-resort failover. This sends the
 * request straight to the coordinating node instead of always hitting the first
 * seed (which funnelled all coordination onto one node). Correctness never
 * depends on routing accuracy: whichever node receives the request coordinates
 * against its own ring, so a stale/empty client ring only costs a forward hop.
 *
 * <p>Connections are pooled per node (Tier 4.4): a healthy socket is returned for
 * reuse after each exchange; a socket that errors is discarded and the exchange
 * retried once on a fresh one (half-open handling).
 */
@Component
public class ClusterClient {

    private static final Logger log = LoggerFactory.getLogger(ClusterClient.class);
    private static final String ORIGIN = "gateway";
    private static final int MAX_FRAME = 64 * 1024 * 1024;  // mirrors framing.h kMaxFrame guard

    private final ClusterProperties props;
    private final RingRouter ringRouter;

    // Per-endpoint ("host:port") pool of idle sockets. Bounded on checkin by
    // maxConnectionsPerNode; surplus sockets are closed rather than hoarded.
    private final Map<String, Deque<Socket>> pools = new ConcurrentHashMap<>();

    public ClusterClient(ClusterProperties props, RingRouter ringRouter) {
        this.props = props;
        this.ringRouter = ringRouter;
    }

    // ---- Public operations --------------------------------------------------

    public WriteResult put(String key, byte[] value, String clock, int n, int w, int r) {
        String payload = String.join("|", "PUT", key, b64(value), ORIGIN,
                Integer.toString(n), Integer.toString(w), Integer.toString(r), nz(clock));
        return parseWrite(exchangeForKey(key, payload, n));
    }

    public WriteResult delete(String key, String clock, int n, int w) {
        String payload = String.join("|", "DELETE", key, ORIGIN,
                Integer.toString(n), Integer.toString(w), nz(clock));
        return parseWrite(exchangeForKey(key, payload, n));
    }

    public ReadResult get(String key, int n, int r) {
        String payload = String.join("|", "GET", key, ORIGIN,
                Integer.toString(n), Integer.toString(r));
        return parseRead(exchangeForKey(key, payload, n));
    }

    public List<RingNode> ring() {
        // No key to hash — the RING snapshot can come from any node.
        return parseRing(exchangeAny("RING|" + ORIGIN));
    }

    // ---- Response parsing ---------------------------------------------------

    private WriteResult parseWrite(String resp) {
        String[] p = resp.split("\\|", -1);
        if (p.length >= 2 && "RESPONSE".equals(p[0]) && "OK".equals(p[1])) {
            return WriteResult.ok(p.length >= 3 ? p[2] : "");
        }
        String reason = p.length >= 3 ? p[2] : "unknown";
        return isQuorumFailure(reason) ? WriteResult.quorumFailed() : WriteResult.error(reason);
    }

    private ReadResult parseRead(String resp) {
        String[] p = resp.split("\\|", -1);
        if (p.length < 2 || !"RESPONSE".equals(p[0])) {
            return ReadResult.error("malformed_response");
        }
        switch (p[1]) {
            case "OK":
                // RESPONSE|OK|b64|clock
                return ReadResult.ok(new ValueVersion(unb64(p[2]), p[3]));
            case "SIBLINGS": {
                // RESPONSE|SIBLINGS|count|b64|clock|b64|clock...
                List<ValueVersion> vs = new ArrayList<>();
                for (int i = 3; i + 1 < p.length; i += 2) {
                    vs.add(new ValueVersion(unb64(p[i]), p[i + 1]));
                }
                return ReadResult.siblings(vs);
            }
            case "NOTFOUND":
                return ReadResult.notFound();
            case "ERROR":
            default:
                String reason = p.length >= 3 ? p[2] : "unknown";
                return isQuorumFailure(reason) ? ReadResult.quorumFailed() : ReadResult.error(reason);
        }
    }

    private List<RingNode> parseRing(String resp) {
        // RING\n<count>\n<id>|<host>|<port>\n...
        List<RingNode> out = new ArrayList<>();
        String[] lines = resp.split("\n");
        if (lines.length == 0 || !"RING".equals(lines[0].trim())) {
            throw new ClusterUnavailableException("unexpected ring response: " + firstLine(resp), null);
        }
        for (int i = 2; i < lines.length; i++) {
            String line = lines[i].trim();
            if (line.isEmpty()) continue;
            String[] f = line.split("\\|", -1);
            if (f.length == 3) {
                try {
                    out.add(new RingNode(f[0], f[1], Integer.parseInt(f[2])));
                } catch (NumberFormatException ignored) {
                    // skip a malformed row rather than fail the whole snapshot
                }
            }
        }
        return out;
    }

    private static boolean isQuorumFailure(String reason) {
        return "quorum_not_met".equals(reason) || "no_nodes_in_ring".equals(reason);
    }

    // ---- Transport: ring-aware target selection + pooled failover -----------

    /** A resolved target endpoint. */
    private record Endpoint(String host, int port) {
        String key() {
            return host + ":" + port;
        }
    }

    /**
     * Ordered targets for a keyed op: the key's ring owners first (primary →
     * secondaries), then any seed node not already covered as a fallback. Before
     * the first ring poll the router is empty, so this is just the seed list —
     * exactly the pre-4.4 behaviour.
     */
    private List<Endpoint> targetsForKey(String key, int n) {
        List<Endpoint> out = new ArrayList<>();
        Set<String> seen = new LinkedHashSet<>();
        for (RingNode owner : ringRouter.ownersFor(key, n)) {
            Endpoint e = new Endpoint(owner.host(), owner.port());
            if (seen.add(e.key())) out.add(e);
        }
        for (Endpoint e : seedEndpoints()) {
            if (seen.add(e.key())) out.add(e);
        }
        return out;
    }

    private List<Endpoint> seedEndpoints() {
        List<Endpoint> out = new ArrayList<>();
        for (String ep : props.getNodes()) {
            int c = ep.lastIndexOf(':');
            out.add(new Endpoint(ep.substring(0, c), Integer.parseInt(ep.substring(c + 1))));
        }
        return out;
    }

    private String exchangeForKey(String key, String payload, int n) {
        return exchange(targetsForKey(key, n), payload);
    }

    private String exchangeAny(String payload) {
        return exchange(seedEndpoints(), payload);
    }

    private String exchange(List<Endpoint> targets, String payload) {
        IOException last = null;
        for (Endpoint e : targets) {
            try {
                return roundTripPooled(e, payload);
            } catch (IOException ex) {
                last = ex;
                log.warn("cluster node {} unreachable: {}", e.key(), ex.toString());
            }
        }
        throw new ClusterUnavailableException("no cluster node reachable", last);
    }

    /**
     * One framed exchange with {@code e}, reusing a pooled socket if one is
     * available. A pooled socket that fails (half-open: the peer closed it while
     * it sat idle) is discarded and the exchange retried once on a fresh socket.
     * Throws IOException only when a *fresh* connection also fails — i.e. the node
     * is genuinely unreachable — so the caller fails over to the next target.
     */
    private String roundTripPooled(Endpoint e, String payload) throws IOException {
        Socket pooled = pollPooled(e);
        if (pooled != null) {
            try {
                String reply = roundTrip(pooled, payload);
                checkin(e, pooled);  // healthy → return for reuse
                return reply;
            } catch (IOException stale) {
                closeQuietly(pooled);  // desynced/half-open → never re-pool
            }
        }
        Socket fresh = dial(e);  // may throw → node unreachable, propagate to failover
        try {
            String reply = roundTrip(fresh, payload);
            checkin(e, fresh);
            return reply;
        } catch (IOException ex) {
            closeQuietly(fresh);
            throw ex;
        }
    }

    private static String roundTrip(Socket s, String payload) throws IOException {
        sendFramed(s.getOutputStream(), payload);
        return recvFramed(s.getInputStream());
    }

    private Socket dial(Endpoint e) throws IOException {
        Socket s = new Socket();
        s.connect(new InetSocketAddress(e.host(), e.port()), props.getConnectTimeoutMs());
        s.setSoTimeout(props.getReadTimeoutMs());
        return s;
    }

    private Socket pollPooled(Endpoint e) {
        Deque<Socket> dq = pools.get(e.key());
        if (dq == null) return null;
        Socket s;
        while ((s = dq.pollFirst()) != null) {
            if (!s.isClosed()) return s;  // best-effort liveness; real check is the round trip
            closeQuietly(s);
        }
        return null;
    }

    private void checkin(Endpoint e, Socket s) {
        Deque<Socket> dq = pools.computeIfAbsent(e.key(), k -> new ConcurrentLinkedDeque<>());
        if (dq.size() < props.getMaxConnectionsPerNode()) {
            dq.offerFirst(s);  // LIFO: keep a small warm set, let the rest be created on demand
        } else {
            closeQuietly(s);
        }
    }

    private static void closeQuietly(Socket s) {
        if (s == null) return;
        try {
            s.close();
        } catch (IOException ignored) {
            // closing a broken socket; nothing to do
        }
    }

    // ---- Framing (unchanged wire codec) -------------------------------------

    private static void sendFramed(OutputStream out, String payload) throws IOException {
        byte[] body = payload.getBytes(StandardCharsets.UTF_8);
        out.write((body.length + "\n").getBytes(StandardCharsets.US_ASCII));
        out.write(body);
        out.flush();
    }

    private static String recvFramed(InputStream in) throws IOException {
        // Read the decimal length line one byte at a time, then exactly len bytes.
        StringBuilder len = new StringBuilder();
        int c;
        while ((c = in.read()) != '\n') {
            if (c < 0) throw new IOException("EOF before frame header");
            if (c < '0' || c > '9') throw new IOException("malformed frame header");
            len.append((char) c);
            if (len.length() > 20) throw new IOException("frame header too long");
        }
        if (len.length() == 0) throw new IOException("empty frame header");
        int n = Integer.parseInt(len.toString());
        if (n < 0 || n > MAX_FRAME) throw new IOException("frame too large: " + n);
        byte[] body = in.readNBytes(n);
        if (body.length != n) throw new IOException("truncated frame: " + body.length + "/" + n);
        return new String(body, StandardCharsets.UTF_8);
    }

    // ---- Small helpers ------------------------------------------------------

    private static String b64(byte[] v) {
        return Base64.getEncoder().encodeToString(v == null ? new byte[0] : v);
    }

    private static byte[] unb64(String s) {
        return Base64.getDecoder().decode(s);
    }

    private static String nz(String s) {
        return s == null ? "" : s;
    }

    private static String firstLine(String s) {
        int nl = s.indexOf('\n');
        return nl < 0 ? s : s.substring(0, nl);
    }
}
