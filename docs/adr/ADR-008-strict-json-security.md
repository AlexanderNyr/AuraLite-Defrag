# ADR-008: Strict JSON Configuration & Scenario Validation

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Implement a self-contained strict recursive-descent JSON parser (`src/core/json.{hpp,cpp}`) rejecting payloads > 1 MiB, nesting depth > 8, duplicate object keys, trailing garbage, invalid UTF-8, unknown schema keys, and path traversal (`..`).
