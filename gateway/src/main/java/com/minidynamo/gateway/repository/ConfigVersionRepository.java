package com.minidynamo.gateway.repository;

import com.minidynamo.gateway.entity.ConfigVersionEntity;
import java.util.Optional;
import org.springframework.data.jpa.repository.JpaRepository;

public interface ConfigVersionRepository extends JpaRepository<ConfigVersionEntity, Long> {

    /** The most recent config version — the currently-effective N/W/R defaults. */
    Optional<ConfigVersionEntity> findFirstByOrderByVersionDesc();
}
