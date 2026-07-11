package com.minidynamo.gateway.repository;

import com.minidynamo.gateway.entity.NodeEntity;
import org.springframework.data.jpa.repository.JpaRepository;

public interface NodeRepository extends JpaRepository<NodeEntity, String> {
}
