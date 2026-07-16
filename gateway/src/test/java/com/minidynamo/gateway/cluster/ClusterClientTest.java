package com.minidynamo.gateway.cluster;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.minidynamo.gateway.config.ClusterProperties;
import com.minidynamo.gateway.support.FakeClusterServer;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

/**
 * Verifies the wire codec: that {@link ClusterClient} frames requests correctly
 * and parses every response shape the cluster can send. Uses a fake framed TCP
 * server, so no C++ cluster is needed.
 */
class ClusterClientTest {

    private ClusterClient clientFor(FakeClusterServer server) {
        ClusterProperties props = new ClusterProperties();
        props.setNodes(List.of(server.endpoint()));
        // Empty ring (never rebuilt) → routing falls back to the seed list, which
        // is exactly what these wire-codec tests exercise.
        return new ClusterClient(props, new RingRouter(128));
    }

    @Test
    void putSendsFramedRequestAndParsesOkClock() throws Exception {
        AtomicReference<String> seen = new AtomicReference<>();
        try (FakeClusterServer server = new FakeClusterServer(req -> {
            seen.set(req);
            return "RESPONSE|OK|node1:5";
        })) {
            WriteResult r = clientFor(server).put("k", "hello".getBytes(StandardCharsets.UTF_8),
                    "node1:4", 3, 2, 2);
            assertThat(r.status()).isEqualTo(WriteResult.Status.OK);
            assertThat(r.clock()).isEqualTo("node1:5");
            // Request is well-formed: PUT|key|b64(hello)|gateway|N|W|R|clock
            String expectedB64 = Base64.getEncoder().encodeToString("hello".getBytes(StandardCharsets.UTF_8));
            assertThat(seen.get()).isEqualTo("PUT|k|" + expectedB64 + "|gateway|3|2|2|node1:4");
        }
    }

    @Test
    void getParsesOkValueAndClock() throws Exception {
        String b64 = Base64.getEncoder().encodeToString("world".getBytes(StandardCharsets.UTF_8));
        try (FakeClusterServer server = new FakeClusterServer(req -> "RESPONSE|OK|" + b64 + "|node2:1")) {
            ReadResult r = clientFor(server).get("k", 3, 2);
            assertThat(r.status()).isEqualTo(ReadResult.Status.OK);
            assertThat(new String(r.values().get(0).value(), StandardCharsets.UTF_8)).isEqualTo("world");
            assertThat(r.values().get(0).clock()).isEqualTo("node2:1");
        }
    }

    @Test
    void getParsesSiblings() throws Exception {
        String a = Base64.getEncoder().encodeToString("a".getBytes(StandardCharsets.UTF_8));
        String b = Base64.getEncoder().encodeToString("b".getBytes(StandardCharsets.UTF_8));
        try (FakeClusterServer server = new FakeClusterServer(
                req -> "RESPONSE|SIBLINGS|2|" + a + "|node1:1|" + b + "|node2:1")) {
            ReadResult r = clientFor(server).get("k", 3, 2);
            assertThat(r.status()).isEqualTo(ReadResult.Status.SIBLINGS);
            assertThat(r.values()).hasSize(2);
            assertThat(new String(r.values().get(0).value(), StandardCharsets.UTF_8)).isEqualTo("a");
            assertThat(new String(r.values().get(1).value(), StandardCharsets.UTF_8)).isEqualTo("b");
        }
    }

    @Test
    void getParsesNotFound() throws Exception {
        try (FakeClusterServer server = new FakeClusterServer(req -> "RESPONSE|NOTFOUND")) {
            assertThat(clientFor(server).get("k", 3, 2).status()).isEqualTo(ReadResult.Status.NOT_FOUND);
        }
    }

    @Test
    void quorumFailureIsRecognised() throws Exception {
        try (FakeClusterServer server = new FakeClusterServer(req -> "RESPONSE|ERROR|quorum_not_met")) {
            assertThat(clientFor(server).put("k", new byte[0], null, 3, 2, 2).status())
                    .isEqualTo(WriteResult.Status.QUORUM_FAILED);
        }
    }

    @Test
    void emptyValueRoundTripsThroughSplit() throws Exception {
        // RESPONSE|OK||clock — an empty value must not collapse the field split.
        try (FakeClusterServer server = new FakeClusterServer(req -> "RESPONSE|OK||node1:1")) {
            ReadResult r = clientFor(server).get("k", 3, 2);
            assertThat(r.status()).isEqualTo(ReadResult.Status.OK);
            assertThat(r.values().get(0).value()).isEmpty();
            assertThat(r.values().get(0).clock()).isEqualTo("node1:1");
        }
    }

    @Test
    void ringIsParsed() throws Exception {
        try (FakeClusterServer server = new FakeClusterServer(
                req -> "RING\n2\nnode1|node1|5001\nnode2|node2|5002\n")) {
            List<RingNode> ring = clientFor(server).ring();
            assertThat(ring).containsExactly(
                    new RingNode("node1", "node1", 5001),
                    new RingNode("node2", "node2", 5002));
        }
    }

    @Test
    void failsOverWhenFirstNodeUnreachable() throws Exception {
        try (FakeClusterServer good = new FakeClusterServer(req -> "RESPONSE|OK|node1:1")) {
            ClusterProperties props = new ClusterProperties();
            // First endpoint is a dead port; client should fail over to the good one.
            props.setNodes(List.of("localhost:1", good.endpoint()));
            props.setConnectTimeoutMs(300);
            ClusterClient client = new ClusterClient(props, new RingRouter(128));
            assertThat(client.put("k", new byte[0], null, 3, 2, 2).status())
                    .isEqualTo(WriteResult.Status.OK);
        }
    }

    @Test
    void throwsWhenNoNodeReachable() {
        ClusterProperties props = new ClusterProperties();
        props.setNodes(List.of("localhost:1"));
        props.setConnectTimeoutMs(300);
        ClusterClient client = new ClusterClient(props, new RingRouter(128));
        assertThatThrownBy(() -> client.get("k", 3, 2))
                .isInstanceOf(ClusterUnavailableException.class);
    }
}
