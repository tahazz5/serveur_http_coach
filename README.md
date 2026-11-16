# Atlas REST — C++20

A dependency-free Linux HTTP/1.1 REST server with a bounded worker pool, JSON tasks API, bearer authentication, pagination, optimistic concurrency, per-IP rate limiting, structured access logs, and graceful shutdown.

## Build and run

Requires Linux, CMake 3.16+, a C++20 compiler, and pthreads. Python 3 is needed for integration tests. No dependency downloads are required.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
export API_TOKEN="$(openssl rand -hex 32)"
./build/atlas-server
```

The server listens on `127.0.0.1:8080`. Configuration:

| Environment variable | Default | Purpose |
| --- | --- | --- |
| `API_TOKEN` | required | At least 16 printable ASCII characters, without spaces |
| `HOST` | `127.0.0.1` | IPv4 bind address |
| `PORT` | `8080` | Port, 1–65535 |
| `WORKERS` | `4` | Worker threads, 1–64 |

## API

All routes except `/health` require `Authorization: Bearer <API_TOKEN>`. Writes use `Content-Type: application/json`. Responses contain JSON, except successful deletion, which has an empty body. Errors use `{"error":"message"}`. Every response includes `X-Request-ID` for correlation with JSON logs on stderr.

| Method | Route | Behavior |
| --- | --- | --- |
| GET | `/health` | Public liveness check |
| GET | `/api/v1/stats` | Request count and current task count |
| GET | `/api/v1/tasks` | Paginated task list |
| POST | `/api/v1/tasks` | Create a task; returns 201, Location and ETag |
| GET | `/api/v1/tasks/{id}` | Fetch task and ETag |
| PATCH | `/api/v1/tasks/{id}` | Change title or completed; increments version |
| DELETE | `/api/v1/tasks/{id}` | Delete task; returns 204 |

Task representation:

```json
{"id":1,"title":"Build the API","completed":false,"version":1}
```

Creation requires `title`, a nonblank UTF-8 string of at most 200 bytes. `completed` is optional and defaults to false. PATCH accepts either or both fields; empty objects, unknown fields, and invalid types are rejected with 422. Invalid JSON, duplicate keys, and invalid UTF-8 produce 400.

List parameters are `limit` (1–100, default 20), `offset` (default 0), and `completed` (`true` or `false`). Results are ordered by increasing ID; `total` is the count after filtering and before pagination. Query names and values use literal ASCII; percent-encoded query parameters are unsupported.

```bash
curl http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/api/v1/tasks \
  -H "Authorization: Bearer $API_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"title":"Build the API"}'
curl 'http://127.0.0.1:8080/api/v1/tasks?limit=10&completed=false' \
  -H "Authorization: Bearer $API_TOKEN"
curl -X PATCH http://127.0.0.1:8080/api/v1/tasks/1 \
  -H "Authorization: Bearer $API_TOKEN" \
  -H 'Content-Type: application/json' -H 'If-Match: "1"' \
  -d '{"completed":true}'
```

PATCH and DELETE accept an optional `If-Match` header containing the exact quoted version from ETag. Stale versions return 409. Without this header writes use last-writer-wins behavior. Stats count requests reaching the router, including failed authentication, but exclude rate-limited and malformed HTTP requests.

## Resource limits and deployment

- Task data is **in memory** and disappears on restart. The task cap is 10,000; capacity exhaustion returns 409. This is a working server foundation, not a durable database service.
- Each peer IP receives a token bucket of 60 requests with a refill of one per second. Excess requests return 429 with Retry-After. Health checks share this limit.
- The pending connection queue holds 128 connections; excess connections are closed. Worker count is fixed. Active clients have two-second socket I/O timeouts and a ten-second read deadline checked between reads.
- Headers are capped at 16 KiB and bodies at 64 KiB. JSON nesting is capped at 32. Duplicate headers, chunked transfer encoding, and Expect are rejected. Connections close after one response; keep-alive and pipelining are unsupported.
- SIGINT and SIGTERM stop acceptance, close queued connections, and let active workers finish within their I/O bounds.
- TLS terminates at a reverse proxy. Use a secret token and a private upstream bind. No CORS policy, multi-user authorization, or TLS is implemented.
- Rate limiting uses the socket peer address and deliberately ignores forwarded IP headers. Behind a proxy, clients share its bucket; configure edge rate limiting and adjust the application policy before public deployment.

## Tests

```bash
ctest --test-dir build --output-on-failure
# Core tests only, for sandboxes that prohibit sockets:
ctest --test-dir build -R '^core$' --output-on-failure
```

Core tests exercise JSON, routing, validation, version conflicts, filtering, rate limiting, and 800 concurrent writes. The Python integration suite starts a real server and checks CRUD, authentication, malformed HTTP, concurrent HTTP calls, rate limits, and graceful termination. It requires permission to create local sockets. To build just the server without Python, configure with `-DBUILD_TESTING=OFF`.
