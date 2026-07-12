// k6 load profile for the Mini Dynamo gateway.
//
// Each VU loops: with probability WRITE_RATIO it PUTs, otherwise it GETs, over a
// fixed key pool. N/W/R are passed as query params so the same script measures
// any quorum setting. Throughput comes from k6's built-in http_reqs rate; PUT and
// GET latencies are tracked in separate trends so their percentiles report apart.
//
// Env knobs (all optional): N, W, R, VUS, DURATION, WRITE_RATIO, KEYSPACE,
// BASE_URL, AUTH_USER, AUTH_PASS. No remote imports — runs fully offline.
import http from 'k6/http';
import { check } from 'k6';
import { Trend, Rate, Counter } from 'k6/metrics';

const N = __ENV.N || '3';
const W = __ENV.W || '2';
const R = __ENV.R || '2';
const VUS = parseInt(__ENV.VUS || '10', 10);
const DURATION = __ENV.DURATION || '30s';
const WRITE_RATIO = parseFloat(__ENV.WRITE_RATIO || '0.3');
const KEYSPACE = parseInt(__ENV.KEYSPACE || '200', 10);
const KEYPREFIX = __ENV.KEYPREFIX || 'load'; // chaos.sh reads its seeded 'chaos-*' keys
const BASE = __ENV.BASE_URL || 'http://gateway:8080';
const USER = __ENV.AUTH_USER || 'admin';
const PASS = __ENV.AUTH_PASS || 'changeme';

const putLatency = new Trend('put_latency', true);
const getLatency = new Trend('get_latency', true);
const writeRejected = new Rate('write_rejected'); // PUTs that didn't return 200 (e.g. quorum not met)
const opErrors = new Counter('op_errors');        // 5xx / unexpected responses

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
  return { token: res.json('token') };
}

export default function (data) {
  const auth = {
    headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${data.token}` },
  };
  const key = `${KEYPREFIX}-${Math.floor(Math.random() * KEYSPACE) + 1}`;

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
    // A key not yet written returns 404 — that's a served response, not an error.
    const served = res.status === 200 || res.status === 404;
    if (!served) opErrors.add(1, { op: 'get', status: String(res.status) });
    check(res, { 'get served (200/404)': () => served });
  }
}
