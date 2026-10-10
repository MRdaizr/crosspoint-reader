# SD plugin port boundaries

Discovery precedence is `/.crosspoint/plugins`, `/plugins`, then `/.plugins`;
FAT case-insensitive names in an earlier root shadow later roots. Names are at
most 23 ASCII identifier characters. At most 16 plugins are exposed.

Installations start disabled. Device info and the native web UI require explicit
trust confirmation. SHA-256 approval covers the root and the existence/content
of `plugin.js`, `main.js`, `device.json`, and `manifest.json` (including versions).
Changing those requires approval again; README/config/token changes do not.
Browser code is trusted same-origin code, not a sandbox. Firmware never runs JS.
Native file/WebDAV routes cannot serve plugin roots or permission records.
The dedicated script, relay, fetch, upload, crypto, and job routes require both
the current web-session capability and live plugin approval.

## Template encoding

Catalogs, auth, sidecars, and device events use one bounded single-pass renderer
(`util/PluginHttpTemplates.h`, maximum input/output 8192 bytes). Values containing
another placeholder are data and are never interpreted a second time.

- URL `{field}` values are UTF-8 byte percent-encoded, including slash, spaces,
  quotes, `&`, `?`, `#`, and newlines. Both `query` and legacy `query_raw` are
  encoded in URL context; they are ordinary escaped strings in JSON context.
- In JSON string literals, `{field}` escapes quotes, backslashes and controls.
  Outside quotes it emits a strict JSON number/boolean/null when the value is
  one of those scalar spellings; otherwise it emits a quoted JSON string.
  To force an ID such as `123` to stay a string, write `"{id}"`.
- `{raw-json:cfg.payload}` explicitly inserts validated JSON only outside a
  JSON string. Invalid JSON, excessive nesting (>16), or >1024 nodes is rejected.
- `{raw-url:cfg.base_url}/books?q={query}` explicitly inserts a validated complete
  HTTP(S) URL. It cannot contain credentials, controls, fragments or spaces.
  Restricted baseline compatibility: an *entire* URL template equal to `{url}`
  may use the feed's already validated complete URL, including XML direct links.
  No other placeholder is implicitly raw, even if it looks like a URL.
- Request bodies beginning with `{`, `[`, or `"` (after whitespace) use JSON
  rules. Other request-body templates encode substituted values as form fields.
  Literal form delimiters are retained (`grant_type=password&username={cfg.user}`).
- Header values keep ordinary text, then reject controls/CRLF and forbidden
  request header names. SD destination/filename/toast contexts keep their text;
  destinations and all temp/backup siblings are checked against protected paths.
- Unknown template keys, invalid explicit extensions, or oversized expansions
  reject the operation, not silently downgrade to raw interpolation.

Event keys are `token`, `cfg.*`, `meta.*`, `event.*`, `event.ts`, `event.id`.
Catalog keys additionally include `page`, `limit`, `query`, `query_raw`,
`device_code`, `id`, `title`, `author`, `url`; sidecars also get `md5` and `dest`.
Example event body: `{"book":"{event.book}","seconds":{event.duration},"id":"{event.id}"}`.

## Limits and integration

Event outboxes are 4096 bytes per plugin, 512 bytes per line, append-only until
acknowledgement. A full outbox rejects new records and retains older ones.
Before appending, a bounded backward scan truncates only a non-newline-terminated
power-loss tail, retaining every complete line. Completely torn files truncate
to zero; files already larger than 4096 bytes are not modified or appended.
Paths exceeding 220 bytes (including temp/backup siblings), variables exceeding
their field caps, or event lines exceeding 512 bytes are skipped/rejected. Long
paths and event payloads are not supported without limit; a queued outbox must
have room for a complete line before a new event can be accepted.
Drain attempts at most four lines globally and stops a plugin on first failure.
Acknowledged-prefix rewrites use temp/backup replacement; a failed rewrite may
replay stable IDs, giving ordered at-least-once delivery. Event HTTP acknowledgments
are <=8KB and event downloads <=1MB. Download replacement invalidates regenerable
book caches only; reading progress and global annotations are retained.

The parent owns network lifecycle. `drain(renderer, maxEvents, deadlineMs,
cancel, context)` uses an absolute `millis()` deadline and cooperative cancellation.
`shouldConnectForSleep(pluginSleepConnect, batteryPercent, hasSavedNetwork)` gates
saved-network connection at >=20% battery; parent defaults the setting to false.
Connection budget is 10s and total sleep budget 20s. DeadlineDns wraps only
guarded NetworkManager calls with asynchronous lwIP resolution and a 5ms polling
quantum (at least one RTOS tick). Four stable static request slots quarantine late
callbacks; an exhausted pool fails closed. Unguarded native network calls retain
the original SDK behavior. TCP/TLS phase timeouts split the remaining budget
conservatively, while streaming and retries check the absolute deadline.
The deadline-aware DNS/transport path is implemented and host-tested, but the
**hard total 20-second device limit is not yet accepted**. HAL/digest reads can
cross a deadline between cooperative checks, and real TLS/SD timing still needs
hardware verification. Non-wolfSSL fallback builds do not use this DNS wrapper.

Web jobs are six web-session-only slots with args/results strictly below 192
UTF-8 JSON bytes, ten-minute reclaimable running leases, and claim tokens to
reject stale completions. Downloads use `.part` and rollback-capable `.bak`
replacement; uploads use `.tmp`. Auth headers/basic credentials are stripped
on cross-origin file redirects, and HTTPS -> HTTP redirects are rejected.
OAuth requests do not auto-follow redirects or replay secret bodies.

TLS uses the current SDK consumers' `setInsecure()` behavior, so certificates
are not authenticated. Origin checks prevent accidental redirect credential
forwarding but do not provide server identity/MITM protection. Response body
caps and header budgets are software bounds, not device heap/TLS validation.
No real-device heap, stack-watermark, SD power-loss, network/auth endpoint, or
four-orientation interaction tests have been performed by this agent.

The approved-plugin crypto API supports random (0..4096 bytes, `len` with `bytes`
alias), SHA-1/SHA-256, AES-128-CBC (PKCS#7 encryption/raw block decryption), raw
RSA private operations, legacy RSA-1024 SPKI/PKCS#8 generation/signing, certificate
public encryption and bounded PKCS#12 identity extraction. Keys/certs are DER,
not PEM. RSA-1024/SHA-1 are upstream compatibility, not a recommendation for new
security protocols. Requests are <=16KiB and decoded fields total <=8KiB;
contexts/buffers are checked heap allocations, wiped after use, with conservative
heap admission and watchdog/yield points. PKCS#12 accepts definite DER key,
shrouded-key and X.509 bags with bounded PBES2/PBKDF2 or PKCS#12 PBE; nested,
secret, CRL, RC4 or signed-data bags fail explicitly. Password conversion retains
the SDK's ASCII/byte semantics. Caps are 8 safes, 16 bags, 10000 iterations per
KDF and 30000 digest rounds per request. WC_RC2 is enabled only for legacy
identity-bundle decryption; TLS policy is unchanged. No DRM/expiry/book-key service
or ContentProtection reader pipeline is imported. Pure codec/DER/KDF boundary
tests do not substitute for real-device crypto/heap/stack measurements.
Multi-file bundle commit is per-file atomic, not whole-directory atomic;
an interrupted bundle requires retry and changes to approval material fail closed.

Parent test registrations: `add_subdirectory(plugins)`,
`add_subdirectory(reader_session)`, `add_subdirectory(plugin_manifest_classification)`.

WebServer now mounts GET `/api/library` and confirmed POST
`/api/library/rebuild` through `LibraryWebApi`, pausing/restoring WebSocket/UDP
for rebuild. FilesPage uses explicit searches, Chinese IME composition guards,
sort/direction, 16-row pagination, text-only JSON rendering, encoded native
download links, and AbortController cancellation. There is no library polling.

## Translation handoff

These new catalog/plugin keys are already present in the parent's English YAML;
the parent owns English/Chinese/Japanese translation updates, not this agent:

```
STR_DOWNLOAD_COMPLETE STR_EVENT_BOOK_CLOSE STR_EVENT_BOOK_OPEN STR_EVENT_DOWNLOAD
STR_EVENT_READING_SESSION STR_EVENT_SLEEP STR_FETCH STR_INSTALLED
STR_NO_PLUGINS_INSTALLED STR_PLUGIN_AUTH_FAILED STR_PLUGIN_AUTH_WAITING
STR_PLUGIN_DISABLE STR_PLUGIN_DISABLED STR_PLUGIN_ENABLE STR_PLUGIN_ENABLED
STR_PLUGIN_MANIFEST_INVALID STR_PLUGIN_NOT_SIGNED_IN STR_PLUGIN_README
STR_PLUGIN_REAPPROVE STR_PLUGIN_RECEIVES_EVENTS STR_PLUGIN_SIGN_IN
STR_PLUGIN_SIGN_IN_HINT STR_PLUGIN_TRUST_WARNING STR_PLUGIN_WEB_ONLY
STR_PLUGINS STR_UPDATE_AVAILABLE
```

No settings symbols need to be added for these sources to compile. The intended
settings are **per-plugin enable/disable** and parent-owned `pluginSleepConnect`
(default false), not a persistent global plugin-enable setting. The optional
`setSystemEnabled(bool)` API is only a volatile runtime kill switch, default true;
no parent setting or lifecycle call to it is required. Parent owns reader/Wi-Fi/
sleep event hooks. Trust persistence is per plugin in the protected SD approval
store. Use the existing local HalClock;
there is no additional NTP/TrustedTime integration.
