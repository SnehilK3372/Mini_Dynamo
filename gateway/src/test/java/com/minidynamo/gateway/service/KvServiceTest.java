package com.minidynamo.gateway.service;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import com.minidynamo.gateway.cluster.ClusterClient;
import com.minidynamo.gateway.cluster.ReadResult;
import com.minidynamo.gateway.cluster.ValueVersion;
import com.minidynamo.gateway.cluster.WriteResult;
import com.minidynamo.gateway.dto.KvValueResponse;
import com.minidynamo.gateway.dto.KvWriteRequest;
import com.minidynamo.gateway.dto.WriteResponse;
import com.minidynamo.gateway.entity.AuditLogEntity;
import com.minidynamo.gateway.exception.ClusterProtocolException;
import com.minidynamo.gateway.exception.KeyNotFoundException;
import com.minidynamo.gateway.exception.QuorumNotMetException;
import com.minidynamo.gateway.exception.SiblingsConflictException;
import com.minidynamo.gateway.repository.AuditLogRepository;
import com.minidynamo.gateway.repository.ConfigVersionRepository;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.mockito.ArgumentCaptor;
import org.mockito.Mock;
import org.mockito.junit.jupiter.MockitoExtension;

@ExtendWith(MockitoExtension.class)
class KvServiceTest {

    @Mock private ClusterClient cluster;
    @Mock private ConfigVersionRepository configRepo;
    @Mock private AuditLogRepository auditRepo;

    private KvService service;

    @BeforeEach
    void setUp() {
        // No config rows → the service falls back to Dynamo's 3/2/2 defaults.
        when(configRepo.findFirstByOrderByVersionDesc()).thenReturn(Optional.empty());
        service = new KvService(cluster, configRepo, auditRepo);
    }

    private static ValueVersion vv(String data, String clock) {
        return new ValueVersion(data.getBytes(StandardCharsets.UTF_8), clock);
    }

    @Test
    void putUsesDefaultQuorumAndReturnsClock() {
        when(cluster.put(eq("k"), any(), eq("ctx"), eq(3), eq(2), eq(2)))
                .thenReturn(WriteResult.ok("node1:1"));
        WriteResponse r = service.put("k", new KvWriteRequest("v", "ctx"), null, null, null);
        assertThat(r.clock()).isEqualTo("node1:1");
    }

    @Test
    void putHonoursExplicitQuorumOverrides() {
        when(cluster.put(eq("k"), any(), any(), eq(5), eq(1), eq(1)))
                .thenReturn(WriteResult.ok("c"));
        service.put("k", new KvWriteRequest("v", null), 5, 1, 1);
        verify(cluster).put(eq("k"), any(), any(), eq(5), eq(1), eq(1));
    }

    @Test
    void putQuorumFailureMapsToException() {
        when(cluster.put(any(), any(), any(), anyInt(), anyInt(), anyInt()))
                .thenReturn(WriteResult.quorumFailed());
        assertThatThrownBy(() -> service.put("k", new KvWriteRequest("v", null), null, null, null))
                .isInstanceOf(QuorumNotMetException.class);
    }

    @Test
    void putClusterErrorMapsToProtocolException() {
        // An upstream protocol/other error (not a quorum miss) surfaces as 502.
        when(cluster.put(any(), any(), any(), anyInt(), anyInt(), anyInt()))
                .thenReturn(WriteResult.error("bad_upstream"));
        assertThatThrownBy(() -> service.put("k", new KvWriteRequest("v", null), null, null, null))
                .isInstanceOf(ClusterProtocolException.class);
    }

    @Test
    void getQuorumFailureMapsToException() {
        when(cluster.get("k", 3, 2)).thenReturn(ReadResult.quorumFailed());
        assertThatThrownBy(() -> service.get("k", null, null))
                .isInstanceOf(QuorumNotMetException.class);
    }

    @Test
    void deleteQuorumFailureMapsToException() {
        // captureBefore reads first (best-effort), then the delete itself misses W.
        when(cluster.get("k", 3, 2)).thenReturn(ReadResult.ok(vv("old", "node1:1")));
        when(cluster.delete("k", null, 3, 2)).thenReturn(WriteResult.quorumFailed());
        assertThatThrownBy(() -> service.delete("k", null, null, null, "admin"))
                .isInstanceOf(QuorumNotMetException.class);
    }

    @Test
    void getReturnsValue() {
        when(cluster.get("k", 3, 2)).thenReturn(ReadResult.ok(vv("hello", "node1:1")));
        KvValueResponse r = service.get("k", null, null);
        assertThat(r.value()).isEqualTo("hello");
        assertThat(r.clock()).isEqualTo("node1:1");
    }

    @Test
    void getNotFoundThrows() {
        when(cluster.get("k", 3, 2)).thenReturn(ReadResult.notFound());
        assertThatThrownBy(() -> service.get("k", null, null))
                .isInstanceOf(KeyNotFoundException.class);
    }

    @Test
    void getSiblingsThrowsWithAllVersions() {
        when(cluster.get("k", 3, 2))
                .thenReturn(ReadResult.siblings(List.of(vv("a", "node1:1"), vv("b", "node2:1"))));
        assertThatThrownBy(() -> service.get("k", null, null))
                .isInstanceOf(SiblingsConflictException.class)
                .satisfies(e -> assertThat(((SiblingsConflictException) e).getSiblings()).hasSize(2));
    }

    @Test
    void deleteWritesAuditRecord() {
        when(cluster.get("k", 3, 2)).thenReturn(ReadResult.ok(vv("old", "node1:1")));
        when(cluster.delete("k", "ctx", 3, 2)).thenReturn(WriteResult.ok("node1:2"));

        WriteResponse r = service.delete("k", "ctx", null, null, "admin");

        assertThat(r.clock()).isEqualTo("node1:2");
        ArgumentCaptor<AuditLogEntity> captor = ArgumentCaptor.forClass(AuditLogEntity.class);
        verify(auditRepo).save(captor.capture());
        AuditLogEntity logged = captor.getValue();
        assertThat(logged.getActor()).isEqualTo("admin");
        assertThat(logged.getAction()).isEqualTo("DELETE");
        assertThat(logged.getTargetKey()).isEqualTo("k");
        assertThat(logged.getBefore()).isEqualTo("old");
    }
}
