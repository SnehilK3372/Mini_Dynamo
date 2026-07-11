package com.minidynamo.gateway;

import static io.restassured.RestAssured.given;
import static org.hamcrest.Matchers.equalTo;
import static org.hamcrest.Matchers.hasSize;
import static org.hamcrest.Matchers.notNullValue;

import com.minidynamo.gateway.config.JwtProperties;
import com.minidynamo.gateway.security.JwtService;
import com.minidynamo.gateway.support.FakeClusterServer;
import com.minidynamo.gateway.support.InMemoryClusterHandler;
import io.restassured.RestAssured;
import io.restassured.http.ContentType;
import java.io.IOException;
import java.util.List;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.web.server.LocalServerPort;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.testcontainers.containers.PostgreSQLContainer;
import org.testcontainers.junit.jupiter.Container;
import org.testcontainers.junit.jupiter.Testcontainers;

/**
 * Full-stack integration test: real HTTP in, real Spring Security + JPA +
 * Flyway + a real PostgreSQL (Testcontainers), and a fake framed cluster server
 * standing in for the C++ nodes. Proves the gateway's contract end to end without
 * needing the native cluster — the real-cluster e2e is the docker-compose flow.
 */
@SpringBootTest(webEnvironment = SpringBootTest.WebEnvironment.RANDOM_PORT)
@Testcontainers
class GatewayIntegrationTest {

    @Container
    static final PostgreSQLContainer<?> POSTGRES =
            new PostgreSQLContainer<>("postgres:16-alpine");

    static final FakeClusterServer CLUSTER;

    // A known secret so the test can mint expired/tampered tokens the running
    // gateway will validate against.
    private static final String JWT_SECRET = "integration-test-secret-0123456789-abcdef";

    static {
        try {
            CLUSTER = new FakeClusterServer(new InMemoryClusterHandler(List.of(
                    "node1|node1|5001", "node2|node2|5002", "node3|node3|5003")));
        } catch (IOException e) {
            throw new IllegalStateException("failed to start fake cluster", e);
        }
    }

    @DynamicPropertySource
    static void properties(DynamicPropertyRegistry registry) {
        registry.add("spring.datasource.url", POSTGRES::getJdbcUrl);
        registry.add("spring.datasource.username", POSTGRES::getUsername);
        registry.add("spring.datasource.password", POSTGRES::getPassword);
        registry.add("cluster.nodes", CLUSTER::endpoint);
        registry.add("jwt.secret", () -> JWT_SECRET);
    }

    private static JwtService jwtService(long expiryMinutes) {
        JwtProperties p = new JwtProperties();
        p.setSecret(JWT_SECRET);
        p.setExpiryMinutes(expiryMinutes);
        return new JwtService(p);
    }

    @AfterAll
    static void stopCluster() throws IOException {
        CLUSTER.close();
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
                .body("token", notNullValue())
                .extract().path("token");
    }

    @Test
    void protectedRouteRejectsMissingToken() {
        given().contentType(ContentType.JSON).body("{\"value\":\"v\"}")
            .when().put("/v1/kv/foo")
            .then().statusCode(401);
    }

    @Test
    void protectedRouteRejectsExpiredToken() {
        String expired = jwtService(-1).issue("admin", List.of("ADMIN"));
        given().header("Authorization", "Bearer " + expired)
            .when().get("/v1/cluster/ring")
            .then().statusCode(401);
    }

    @Test
    void protectedRouteRejectsTamperedToken() {
        String token = jwtService(30).issue("admin", List.of("ADMIN"));
        // Mutate the FIRST signature char (high-order bits) so the tamper is
        // deterministic — flipping the last base64url char can toggle only an
        // insignificant padding bit and leave the signature valid.
        int sig = token.lastIndexOf('.') + 1;
        char c = token.charAt(sig);
        String tampered = token.substring(0, sig) + (c == 'A' ? 'B' : 'A') + token.substring(sig + 1);
        given().header("Authorization", "Bearer " + tampered)
            .when().get("/v1/cluster/ring")
            .then().statusCode(401);
    }

    @Test
    void badCredentialsAreRejected() {
        given().contentType(ContentType.JSON)
                .body("{\"username\":\"admin\",\"password\":\"wrong\"}")
            .when().post("/v1/auth/token")
            .then().statusCode(401);
    }

    @Test
    void putGetDeleteLifecycle() {
        String jwt = token();

        // PUT
        given().header("Authorization", "Bearer " + jwt)
                .contentType(ContentType.JSON).body("{\"value\":\"hello\"}")
            .when().put("/v1/kv/greeting")
            .then().statusCode(200).body("clock", notNullValue());

        // GET returns what we wrote
        given().header("Authorization", "Bearer " + jwt)
            .when().get("/v1/kv/greeting")
            .then().statusCode(200).body("value", equalTo("hello"));

        // DELETE, then GET is 404 (tombstone converged in the fake)
        given().header("Authorization", "Bearer " + jwt)
            .when().delete("/v1/kv/greeting")
            .then().statusCode(200);

        given().header("Authorization", "Bearer " + jwt)
            .when().get("/v1/kv/greeting")
            .then().statusCode(404);
    }

    @Test
    void missingKeyIs404() {
        given().header("Authorization", "Bearer " + token())
            .when().get("/v1/kv/does-not-exist")
            .then().statusCode(404);
    }

    @Test
    void ringReflectsTheCluster() {
        given().header("Authorization", "Bearer " + token())
            .when().get("/v1/cluster/ring")
            .then().statusCode(200).body("$", hasSize(3))
            .body("id", equalTo(List.of("node1", "node2", "node3")));
    }

    @Test
    void nodesRegistrySyncedFromRingOnStartup() {
        // The ApplicationReadyEvent sync populated the Postgres registry from the
        // fake cluster's ring, so /v1/cluster/nodes returns the three nodes.
        given().header("Authorization", "Bearer " + token())
            .when().get("/v1/cluster/nodes")
            .then().statusCode(200).body("$", hasSize(3));
    }
}
