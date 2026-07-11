package com.minidynamo.gateway;

import static io.restassured.RestAssured.given;
import static org.hamcrest.Matchers.equalTo;
import static org.hamcrest.Matchers.notNullValue;

import io.restassured.RestAssured;
import io.restassured.http.ContentType;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIfEnvironmentVariable;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.web.server.LocalServerPort;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.testcontainers.containers.GenericContainer;
import org.testcontainers.containers.PostgreSQLContainer;
import org.testcontainers.containers.wait.strategy.Wait;
import org.testcontainers.junit.jupiter.Container;
import org.testcontainers.junit.jupiter.Testcontainers;
import org.testcontainers.utility.DockerImageName;

/**
 * Integration test that drives the gateway's HTTP API against the <b>real C++
 * node</b> running in a container (plus a real PostgreSQL), rather than the
 * in-JVM {@code FakeClusterServer} used by {@link GatewayIntegrationTest}. This
 * is what proves the gateway actually speaks the cluster's on-the-wire protocol —
 * length-prefixed framing, base64 values, vector-clock tokens — against the
 * genuine implementation, which a hand-written fake can only approximate.
 *
 * <p>A single bootstrap node is used (its ring has one owner), so requests run
 * with {@code N=W=R=1}; multi-node availability under failure is covered by the
 * docker-compose e2e ({@code scripts/e2e.sh}), not here.
 *
 * <p>Requires the node image to exist; its tag comes from the {@code NODE_IMAGE}
 * env var (CI builds it and sets it). When unset the whole class is skipped, so a
 * plain {@code mvn test} stays hermetic and fast.
 */
@SpringBootTest(webEnvironment = SpringBootTest.WebEnvironment.RANDOM_PORT)
@Testcontainers
@EnabledIfEnvironmentVariable(named = "NODE_IMAGE", matches = ".+")
class RealClusterIT {

    private static final String NODE_IMAGE =
            System.getenv().getOrDefault("NODE_IMAGE", "mini-dynamo-node:ci");

    @Container
    static final PostgreSQLContainer<?> POSTGRES =
            new PostgreSQLContainer<>("postgres:16-alpine");

    // A single bootstrap node: no BOOTSTRAP_IP, in-memory storage (no RocksDB
    // dir needed), TCP protocol on 5001. Wait for the listening socket before the
    // gateway tries to sync the ring.
    @Container
    static final GenericContainer<?> NODE =
            new GenericContainer<>(DockerImageName.parse(NODE_IMAGE))
                    .withEnv("NODE_ID", "node1")
                    .withEnv("NODE_PORT", "5001")
                    .withEnv("STORAGE_ENGINE", "memory")
                    .withExposedPorts(5001)
                    .waitingFor(Wait.forListeningPort());

    @DynamicPropertySource
    static void properties(DynamicPropertyRegistry registry) {
        registry.add("spring.datasource.url", POSTGRES::getJdbcUrl);
        registry.add("spring.datasource.username", POSTGRES::getUsername);
        registry.add("spring.datasource.password", POSTGRES::getPassword);
        registry.add("cluster.nodes", () -> NODE.getHost() + ":" + NODE.getMappedPort(5001));
    }

    @LocalServerPort
    int port;

    @BeforeEach
    void setUp() {
        RestAssured.port = port;
    }

    private String token() {
        return given()
                .contentType(ContentType.JSON)
                .body("{\"username\":\"admin\",\"password\":\"changeme\"}")
            .when()
                .post("/v1/auth/token")
            .then()
                .statusCode(200)
                .extract().path("token");
    }

    @Test
    void putGetDeleteAgainstRealNode() {
        String jwt = token();

        // PUT with W=1 (single-node ring): the node coordinates, writes locally,
        // and returns a real vector clock it stamped (e.g. node1:1).
        given().header("Authorization", "Bearer " + jwt)
                .contentType(ContentType.JSON).body("{\"value\":\"real-cluster\"}")
            .when().put("/v1/kv/itkey?N=1&W=1&R=1")
            .then().statusCode(200).body("clock", notNullValue());

        // GET decodes the base64 value off the wire back to the original string.
        given().header("Authorization", "Bearer " + jwt)
            .when().get("/v1/kv/itkey?N=1&R=1")
            .then().statusCode(200).body("value", equalTo("real-cluster"));

        // DELETE writes a real tombstone; the subsequent GET reads it back as 404.
        given().header("Authorization", "Bearer " + jwt)
            .when().delete("/v1/kv/itkey?N=1&W=1")
            .then().statusCode(200);

        given().header("Authorization", "Bearer " + jwt)
            .when().get("/v1/kv/itkey?N=1&R=1")
            .then().statusCode(404);
    }

    @Test
    void ringReflectsTheRealNode() {
        given().header("Authorization", "Bearer " + token())
            .when().get("/v1/cluster/ring")
            .then().statusCode(200).body("id", equalTo(java.util.List.of("node1")));
    }
}
