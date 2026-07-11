package com.minidynamo.gateway.security;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.minidynamo.gateway.config.JwtProperties;
import io.jsonwebtoken.JwtException;
import java.util.List;
import org.junit.jupiter.api.Test;

/** Unit tests for the JWT issue/validate logic (no Spring context). */
class JwtServiceTest {

    private static final String SECRET = "0123456789012345678901234567890123456789";

    private JwtService service(long expiryMinutes) {
        JwtProperties props = new JwtProperties();
        props.setSecret(SECRET);
        props.setExpiryMinutes(expiryMinutes);
        return new JwtService(props);
    }

    @Test
    void issuedTokenRoundTrips() {
        JwtService svc = service(30);
        String token = svc.issue("admin", List.of("ADMIN"));
        var claims = svc.parse(token);
        assertThat(claims.getSubject()).isEqualTo("admin");
        assertThat(claims.get("roles", List.class)).containsExactly("ADMIN");
    }

    @Test
    void expiredTokenIsRejected() {
        // Negative expiry mints a token whose expiration is already in the past.
        JwtService svc = service(-1);
        String expired = svc.issue("admin", List.of("ADMIN"));
        assertThatThrownBy(() -> svc.parse(expired)).isInstanceOf(JwtException.class);
    }

    @Test
    void tamperedTokenIsRejected() {
        JwtService svc = service(30);
        String token = svc.issue("admin", List.of("ADMIN"));
        // Flip the FIRST character of the signature segment. The last base64url
        // char of a 256-bit HS256 signature only carries 4 significant bits (43
        // chars * 6 = 258 bits > 256), so flipping it can leave the decoded bytes
        // unchanged and the token still valid; the first char always encodes
        // high-order bits, so mutating it is a guaranteed, deterministic tamper.
        int sig = token.lastIndexOf('.') + 1;
        char c = token.charAt(sig);
        String tampered = token.substring(0, sig) + (c == 'A' ? 'B' : 'A') + token.substring(sig + 1);
        assertThatThrownBy(() -> svc.parse(tampered)).isInstanceOf(JwtException.class);
    }

    @Test
    void tokenSignedWithAnotherSecretIsRejected() {
        String token = service(30).issue("admin", List.of("ADMIN"));
        JwtProperties other = new JwtProperties();
        other.setSecret("ABCDEFGHABCDEFGHABCDEFGHABCDEFGH12345678");
        JwtService otherSvc = new JwtService(other);
        assertThatThrownBy(() -> otherSvc.parse(token)).isInstanceOf(JwtException.class);
    }
}
