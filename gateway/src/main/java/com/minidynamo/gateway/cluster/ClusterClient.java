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
import java.util.List;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

/**
 * Speaks the C++ cluster's TCP wire protocol from Java. This is the entire
 * gateway&harr;cluster boundary (build-plan 1B.5): length-prefixed framing
 * (matches {@code src/net/framing.cpp}), base64-encoded values (matches {@code
 * src/base64.cpp}), pipe-delimited fields.
 *
 * <p>A fresh socket is opened per request (no pooling — simple and correct for
 * this scope) and the configured nodes are tried in order, failing over on a
 * connection error. Any node can coordinate a request, so which one we reach
 * does not affect correctness.
 *
 * <p>Wire messages produced here:
 * <pre>
 *   PUT|key|b64value|origin|N|W|R|clock   -> RESPONSE|OK|clock | RESPONSE|ERROR|reason
 *   GET|key|origin|N|R                     -> RESPONSE|OK|b64|clock | RESPONSE|SIBLINGS|n|b64|clock... | RESPONSE|NOTFOUND | RESPONSE|ERROR|reason
 *   DELETE|key|origin|N|W|clock            -> RESPONSE|OK|clock | RESPONSE|ERROR|reason
 *   RING|origin                            -> RING\n&lt;count&gt;\n id|host|port\n...
 * </pre>
 */
@Component
public class ClusterClient {

    private static final Logger log = LoggerFactory.getLogger(ClusterClient.class);
    private static final String ORIGIN = "gateway";
    private static final int MAX_FRAME = 64 * 1024 * 1024;  // mirrors framing.h kMaxFrame guard

    private final ClusterProperties props;

    public ClusterClient(ClusterProperties props) {
        this.props = props;
    }

    // ---- Public operations --------------------------------------------------

    public WriteResult put(String key, byte[] value, String clock, int n, int w, int r) {
        String payload = String.join("|", "PUT", key, b64(value), ORIGIN,
                Integer.toString(n), Integer.toString(w), Integer.toString(r), nz(clock));
        return parseWrite(exchange(payload));
    }

    public WriteResult delete(String key, String clock, int n, int w) {
        String payload = String.join("|", "DELETE", key, ORIGIN,
                Integer.toString(n), Integer.toString(w), nz(clock));
        return parseWrite(exchange(payload));
    }

    public ReadResult get(String key, int n, int r) {
        String payload = String.join("|", "GET", key, ORIGIN,
                Integer.toString(n), Integer.toString(r));
        return parseRead(exchange(payload));
    }

    public List<RingNode> ring() {
        String resp = exchange("RING|" + ORIGIN);
        return parseRing(resp);
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

    // ---- Transport: framed request/response with node failover --------------

    private String exchange(String payload) {
        IOException last = null;
        for (String endpoint : props.getNodes()) {
            String host = endpoint.substring(0, endpoint.lastIndexOf(':'));
            int port = Integer.parseInt(endpoint.substring(endpoint.lastIndexOf(':') + 1));
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress(host, port), props.getConnectTimeoutMs());
                s.setSoTimeout(props.getReadTimeoutMs());
                sendFramed(s.getOutputStream(), payload);
                return recvFramed(s.getInputStream());
            } catch (IOException e) {
                last = e;
                log.warn("cluster node {} unreachable: {}", endpoint, e.toString());
            }
        }
        throw new ClusterUnavailableException("no cluster node reachable", last);
    }

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
