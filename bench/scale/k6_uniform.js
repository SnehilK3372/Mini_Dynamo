// Uniform workload for the Tier 4.5 scaling benchmark.
//
// Deliberately uniform over a large keyspace: the point is to measure how the
// CLUSTER scales as nodes are added, so every node should get a proportional
// share of the keys. (bench/load.js uses a small keyspace to exercise conflicts
// and read repair — the opposite goal.) 70/30 read/write per the plan.
//
// Env: N, W, R, VUS, DURATION, WRITE_RATIO, KEYSPACE, BASE_URL, AUTH_USER, AUTH_PASS.
// No remote imports — runs fully offline.
import http from 'k6/http';
import { check } from 'k6';
import { Trend, Rate, Counter } from 'k6/metrics';

const N = __ENV.N || '3';
const W = __ENV.W || '2';
const R = __ENV.R || '2';
const VUS = parseInt(__ENV.VUS || '100', 10);
const DURATION = __ENV.DURATION || '60s';
const WRITE_RATIO = parseFloat(__ENV.WRITE_RATIO || '0.3');
const KEYSPACE = parseInt(__ENV.KEYSPACE || '100000', 10);
const BASE = __ENV.BASE_URL || 'http://nginx';
const USER = __ENV.AUTH_USER || 'admin';
const PASS = __ENV.AUTH_PASS || 'changeme';

const putLatency = new Trend('put_latency', true);
const getLatency = new Trend('get_latency', true);
const writeRejected = new Rate('write_rejected');
const opErrors = new Counter('op_errors');

export const options = {
  vus: VUS,
  duration: DURATION,
  summaryTrendStats: ['avg', 'med', 'p(50)', 'p(95)', 'p(99)', 'max'],
};

export function setup() {
  const res = http.post(
    `${BASE}/v1/auth/token`,
    JSON.stringify({ username: USER, password: PASS }),
    { headers: { 'Content-Type': 'application/json' } }
  );
  check(res, { 'token issued': (r) => r.status === 200 });
  if (res.status !== 200) {
    throw new Error(`auth failed (HTTP ${res.status}) — check AUTH_USER/AUTH_PASS`);
  }
  return { token: res.json('token') };
}

export default function (data) {
  const auth = {
    headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${data.token}` },
  };
  // Uniform over the whole keyspace → uniform over the ring → uniform over nodes.
  const key = `scale-${Math.floor(Math.random() * KEYSPACE) + 1}`;

  if (Math.random() < WRITE_RATIO) {
    const res = http.put(
      `${BASE}/v1/kv/${key}?N=${N}&W=${W}&R=${R}`,
      JSON.stringify({ value: `v-${Date.now()}` }),
      auth
    );
    putLatency.add(res.timings.duration);
    writeRejected.add(res.status !== 200);
    if (res.status >= 500) opErrors.add(1, { op: 'put', status: String(res.status) });
    check(res, { 'put ok': (r) => r.status === 200 });
  } else {
    const res = http.get(`${BASE}/v1/kv/${key}?N=${N}&R=${R}`, auth);
    getLatency.add(res.timings.duration);
    // A key not yet written returns 404 — a served response, not an error. With a
    // 100k keyspace most early reads are 404s; that is expected and still measures
    // the full read path (auth → routing → quorum).
    const served = res.status === 200 || res.status === 404;
    if (!served) opErrors.add(1, { op: 'get', status: String(res.status) });
    check(res, { 'get served (200/404)': () => served });
  }
}
