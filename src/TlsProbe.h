#pragma once

// Diagnostic-only TLS/HTTP probe. Not used by any product path.
//
// `host` is a DNS name (e.g. "m.jjwxc.net" or "www.jjwxc.net"). The probe runs
// one handshake on a PSRAM-stack task at the current internal-heap state and
// logs the DNS+TCP / TLS split. `ballastKb` optionally holds an internal-heap
// block for the duration of the probe so the same code can be tested at a
// lower headroom without rebuilding.
void runTlsProbe(const char* host, int ballastKb);

// Same handshake, then one HTTP/1.1 GET. `path` must not contain spaces and is
// expected to be path+query only (the Host header is built from `host`).
// `useSid` sends only the sid cookie; `useFull` sends the whole device jar.
// `keepAlive` switches Connection from close to keep-alive. The full path is
// never logged (it may carry a token); only the part before '?' is printed.
void runHttpProbe(const char* host, const char* path, bool useSid, bool useFull, bool keepAlive);
