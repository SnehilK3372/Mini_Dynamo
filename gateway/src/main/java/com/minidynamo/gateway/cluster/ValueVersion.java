package com.minidynamo.gateway.cluster;

/**
 * One version of a value as it came back from the cluster: the decoded bytes and
 * the vector-clock token ("node:count,node:count") that versions it. A read
 * returns one of these (dominant version) or several (concurrent siblings).
 */
public record ValueVersion(byte[] value, String clock) {
}
