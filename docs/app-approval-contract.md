# URnetwork app approval contract, version 1

Purpose. A signed-out URnetwork desktop app asks a web page to sign it in. A person who is already signed in on that page approves. The page mints a one-use auth code with that person's own session, seals it to a key only the app holds, and hands it back through a fixed custom-scheme address. The app redeems it with the existing `POST /auth/code-login`. No server change.

Status. Proposal for review, as of 2026-10-09. MUST, MUST NOT, SHOULD and MAY are normative. First implementers: the Windows app and the web manager route `/login-app`; second: the ur.io page `/app/approve`. The extension page `/login-extension` is a different contract and stays as it is.

Versions. Version 1 (sections 1 to 10) is the official network. Version 2 (section 11, added 2026-10-11) is a request that names its network, so the same page can approve a sign-in for a beta or self-hosted network.

## 1. Constants

| Name | Value |
| --- | --- |
| Page URL | A build constant of the app. v1: `https://beta.app.ur.network/login-app`. A second host is a second constant (planned: `https://ur.io/app/approve`). The app never takes a page URL from a server, a file, a network space or a request. |
| Descriptor | A second build constant, fetched with GET and never redirected: `https://beta.app.ur.network/approval-contract.json`, `Content-Type: application/json`, body `{"contracts":[1,2],"platforms":["windows"]}`. The app shows the action only if, in this run, it fetched and parsed this within 3 s and both its platform and the version it would send (1 in the official network, 2 in another) are listed (cached per run, failure included). Extra keys are ignored. |
| Platform table | Closed, inside the page; nothing in a request can add or change a row. `windows` -> label `URnetwork for Windows`, return base `urnetwork://auth`. Another app's row is added only when that app implements section 5. |
| API | Version 1: the official network, the page's default server (`https://api.bringyour.com`); if the browser holds a custom API base the page does not run the approval but shows a 'Different API server' card whose one action is 'Use the default server' (section 7), and the app offers version 1 only in the official network space. Version 2 (section 11): any other network whose API is `https://host[:port]`; the request names it, and the page works only when its browser is set to that server; when it is set to another, the same card offers Cancel and 'Switch to {host}'. |

## 2. Request (app -> page)

Opened in the default browser. Parameters travel only in the fragment, so no server, proxy or Referer receives them:

    <page URL>#v=1&platform=windows&state=<S>&pk=<PK>&ts=<T>

| Key | Value |
| --- | --- |
| `v` | `1` |
| `platform` | an own key of the platform table |
| `state` | 43 characters of `[A-Za-z0-9_-]`: base64url of 32 bytes from the OS random source, fresh per attempt |
| `pk` | base58 of a 32-byte X25519 public key (43 or 44 characters), fresh per attempt; the secret key never leaves the app |
| `ts` | 10 digits, unix seconds at attempt start |

- Exactly these five keys, each once, any order. Unknown, duplicate or empty keys, any `%`, `+`, whitespace or control character, or more than 400 characters make the whole request invalid.
- The page checks shapes and echoes `state`; it cannot verify it. Only the app compares it.
- The page treats a request as stale when `ts` is older than 240 s or more than 120 s ahead, at load and again at Approve.

## 3. Return (page -> app)

Built by the page from the table's return base plus URL-safe values and reached by one navigation (`location.assign`). The page navigates to nothing else.

| Case | Address |
| --- | --- |
| Approve | `urnetwork://auth?state=<S>&epk=<E>&n=<N>&d=<D>` |
| Cancel | `urnetwork://auth?state=<S>&error=access_denied` |

- `E`: the page's one-time X25519 public key. `N`: a random 24-byte nonce. `D`: NaCl box (curve25519-xsalsa20-poly1305; `nacl.box` in tweetnacl, `box.Seal` in golang.org/x/crypto) of the UTF-8 JSON `{"c":"<auth code>","s":"<S>"}`, from the page's one-time secret key to `pk`. `E`, `N` and `D` are base58. Fresh key pair and nonce per message, wiped afterwards.
- Every value matches `[A-Za-z0-9_-]+`, so nothing is percent-encoded and the app MUST reject `%`. `error` has one legal value, `access_denied`.
- The page never emits a path, port, userinfo or fragment. The app MUST accept exactly one `/` between `auth` and `?` (a shell or browser may add it) and nothing else after `auth`.
- Budget: at most 2,000 characters for the whole address (about 1,190 today with the server's 684-character code). The page checks this with a dry run before it mints, so a code is never minted for an address that cannot be built. A longer code or an extra payload field needs a new version.

## 4. Page rules

1. Before the host's sign-in logic runs, parse the fragment. Valid: store it in one session-storage slot (replacing any older one) and rewrite the address to the bare path. Invalid: clear the slot and show a generic 'This sign-in link isn't valid' card. A request is valid only if the page address has no query string: the host may act on query parameters of its own (the manager signs in with `?auth_code=`), and an approval must never ride on one. A load with no fragment keeps the slot (it is the return from sign-in), but only for a tab that this page itself sent to sign in.
2. Interactive only in a top-level window with no opener, decided before first paint; otherwise a non-interactive 'Open this page in its own tab' message.
3. Needs a signed-in session on the host. Signed out: sign in, then come back to the card. A rejected session (HTTP 401) at mint means sign in again.
4. The card MUST show: the app label from the table (never from the request); the network name of the session being approved, as plain text; what approval grants ('full access to this account, the same as signing in there yourself: balance, plans, payout wallet and settings'); that signing out of the page does not sign that computer out; and a 'Not {network}? Sign out and use a different account' action. Cancel comes before Approve in tab order. Approve MUST NOT be auto-focused, SHOULD be inert for about 800 ms after the card is visible and the window has focus (and again whenever the account on the card changes), and acts only on a trusted click.
5. Approve: re-read the session and compare its network identity with the displayed one (different: mint nothing, re-render). Single-flight, one mint per request. `POST /auth/code-create {"uses":1,"duration_minutes":2}` with that session; seal; navigate to the Approve address; clear the slot. SHOULD keep the sealed address in memory for at most 60 s behind an 'Open URnetwork' button (a fresh user click), then drop it.
6. Cancel: navigate to the access_denied address; clear the slot.
7. The code MUST NOT reach the DOM, web storage, logs, analytics or any URL except as ciphertext. The page MUST NOT read any redirect-like parameter (`redirect_uri`, `redirect_link`, `return`, `callback`, `next`), use `postMessage`, or load third-party script on this route. It MAY log one fixed line (action, platform).

## 5. App rules

1. Start only from a user click, only signed out, only in a network a request can name (the official one, version 1; another whose API url section 11 allows, version 2) and after the descriptor check lists the version that request is. One attempt holds state, key pair, `ts`, the `api` it names (version 2), a flow number and a deadline 300 s after start on both the steady and the wall clock; in memory only; a second click re-opens the same request instead of starting another, but only while the page would still accept it (200 s after `ts`, inside the page's 240 s window); later it starts a new attempt, which replaces the old one. Make sure the per-user `urnetwork://` handler points at the running exe (read it back) before opening the browser; if that fails, fall back to the paste flow.
2. Gate every `urnetwork://auth` launch on the UI thread, in this order: strict parse (exact prefix, one optional `/`, the exact key set of one shape, no duplicates, no `%`, length caps); signed in: ignore; no live attempt (none, or past its deadline on either clock, which wipes it): stray; different `state`: stray; `error=access_denied`: consume and show 'cancelled'; otherwise unseal with the attempt's secret key (catch every SDK exception) and require inner `s` == `state`, else stray with the attempt left alive; then consume, raise the window, redeem.
3. A stray MUST NOT raise a window, reset a screen or disturb a live attempt; it logs a closed token. One exception: the first launch of a freshly started process MAY show the window once with a neutral 'That sign-in link is no longer active' line.
4. Redeem with `POST /auth/code-login {"auth_code":...}` (no Authorization), and only while no network change or other sign-in flow has begun since the attempt did (section 11). Success needs a non-empty `by_jwt`; HTTP 200 with `error` (message 'Invalid auth code.') is a refusal. Hold the credential unregistered and unstored until the person confirms 'Sign in to {network}?' using the `network_name` claim (sanitized text). No, or a closed sheet, discards it. Then register the device, persist, and say 'Signed in as {network}'. Never retry a redeem.
5. The attempt ends and its secret is wiped on match, cancel, timeout (the timer also tells an open sheet), any other sign-in starting or succeeding, logout, a network-space change and quit.
6. MUST NOT log a `urnetwork://auth` URL, the command line carrying one, `state`, `pk`, `epk`, `n`, `d` or the code (scheme, host and length at most). `Copy link`, if offered, uses clipboard history and roaming off and clears after 60 s if unchanged; a field for a pasted code is a password box.

## 6. Timing

| What | Value |
| --- | --- |
| Auth code | 1 use, 2 minutes |
| App attempt | 5 minutes, not extended by re-opening |
| Page request window | 240 s after `ts`, 120 s ahead; at load and at Approve |
| Approve inert delay | about 800 ms |
| Sealed address kept by the page | at most 60 s |
| Descriptor check | 3 s timeout, once per run |

## 7. Cases

Page side:

| Situation | Page does | App receives |
| --- | --- | --- |
| Request missing, malformed, unknown platform, or a query string on the page address | Generic 'isn't valid' card; stores nothing; no Approve | nothing |
| `v` is not 1 or 2 | 'Update the app'; no Approve | nothing |
| Request stale | 'This request has expired. Start again from URnetwork.' | nothing |
| Framed, or has an opener | Non-interactive message | nothing |
| The browser's API server is not the one the request is for (version 1: not the default server; version 2: not the host `api` names) | Before any sign-in redirect and before any call to any API: 'Different API server', the hosts as plain text. Version 1: 'Use the default server', which signs this browser out of the server it leaves, removes the override and goes on to the sign-in. Version 2: Cancel, and 'Switch to {host}' behind 'Only continue if you set this server in URnetwork yourself.' (signs this browser out, points it at `https://{host}`, signs in, back to the card) | nothing, or `access_denied` after Cancel |
| Signed out | Sign-in, then the card; the stored request survives | nothing |
| Session rejected at mint (401) | Clears the session; sign in again | nothing |
| Identity differs between card and click | Mints nothing; shows an 'Account changed' card with a Reload button | nothing |
| Account has no sign-in method (a guest) | 'No sign-in method' card; no Approve | nothing |
| Public key `pk` is a small-order point, or sealing cannot be done | Generic 'isn't valid' card and the slot is cleared; for a sealing script that fails to load or hangs (15 s), 'Reload this page and approve again' with Cancel still available | nothing |
| Code limit reached / other mint failure | 'Too many approvals are pending. Try again later.' / 'We couldn't approve this right now. Try again.' | nothing |
| Cancel | Access_denied address; 'Cancelled. Nothing was shared.' | `error=access_denied` |
| Approve | Approve address; 'Approved. Return to URnetwork to finish signing in.' plus Open URnetwork | sealed return |

App side:

| Situation | App does |
| --- | --- |
| Signed in | Ignore (logged) |
| No live attempt, other state, unknown shape, bad seal, inner state differs | Stray: nothing visible, attempt untouched (a fresh process may show the window once) |
| Deadline passed | Attempt wiped; open sheet says 'Browser sign-in timed out'; a later return is a stray |
| access_denied for the live state | Consume; 'Sign-in was cancelled in the browser.' |
| Redeem refused or transport failure | 'That approval expired or was already used. Try again.' / 'Couldn't reach URnetwork. Start again.'; never retried |
| A network change (or any other sign-in flow) begins after the answer was accepted | Before the code is sent: nothing is sent, and the redeem is refused like a spent code. While the redeem is out: what it brings back is dropped, not held for a confirm, and the redeem is refused likewise |
| Confirm declined or sheet swept | Credential discarded; nothing registered |
| Registration refused after the code is spent | Show the server's text; start again |

Wording as built in the Windows app (2026-10-11). The texts above are the intended ones; they need localization keys that the store tags for windows, and the store is a separate backlog. Until then the app uses keys that exist: a denial returns the sheet quietly to its start page, a timeout says the store's `sso_sign_in_timed_out`, and a refused redeem, a stray's single neutral line and the line for an approved answer that has to be dropped (another sheet cannot be closed) all say `auth_code_error`. With the build flag off the sheet is the paste flow, and its one button reads 'Continue in browser' (the id `adv_open_ur_io` is re-worded; no new key). Only words differ; every rule in section 5 holds either way.

## 8. Versioning

`v` is an integer. A page or app that does not know the `v` it receives fails closed ('Update the app'). New keys, new return shapes or different crypto need a new `v`. `v=2` is the request that names its network (section 11). `v=3` is reserved for a server-checked code challenge (needs server work). The descriptor lists the versions a host serves. Platform rows are only ever added.

## 9. Conformance vectors (both repositories commit the same ones)

- Parser accept: a well-formed request with and without the leading `#`; a return with and without the optional `/`; a version 2 request with `api=api.beta-test.net` and with `api=api.example.com:8443`.
- Parser reject, each by name: unknown keys including `redirect_uri`, `redirect_link`, `return`, `callback`, `auth_code`, `next`, `code`, `error`, and case variants; duplicate `state` or `v`; a missing key; `%XX`, `+`, space, NUL, `&&`, trailing `&`; 20,000 characters; `v=3` and `v=01`; `v=2` without `api`; `v=1` with `api`; `api` values that are empty, upper-case, hold a scheme, path, userinfo, port `443`, port `0`, port `65536`, a leading-zero port, a single label, an all-digit last label, a last label of `0x` and hex digits (`a.0x10`, `1.2.3.0x4`, `0x7f.0x0.0x0.0x1`, `a.0x`), an empty or hyphen-edged label, a trailing dot, a non-ASCII character or more than 200 characters; platform `linux`, `__proto__`, `constructor`, `toString`, `hasOwnProperty`, empty; `state` of 42 or 44 characters or containing `.`; `pk` containing `0 O I l`, too short, too long, or not 32 bytes; `ts` non-numeric, 9 digits, 241 s old, 121 s ahead; any query string on the page address.
- Seal vector, fixed non-secret test keys: app secret key = bytes 1..32; page one-time secret key = bytes 101..132; nonce = bytes 200 down to 177; plaintext `{"c":"<683 'A' then '='>","s":"<43 'S'>"}`. Expect `D` of 1,036 base58 characters. The page library must reproduce the committed `epk`, `n`, `d`; the app opens it with `urnet::generateSharedSecret` and `urnet::decryptData`; a flipped byte or a wrong key MUST fail.
- Request vector (version 2), fixed non-secret values: the app builds, from state = 43 'S', the public key of the app secret key above (bytes 1..32), `ts` = 1790000000 and `api` = api.beta-test.net, exactly

      v=2&platform=windows&state=SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS&pk=WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf&ts=1790000000&api=api.beta-test.net

  (153 characters, the keys in this order) after the page URL and `#`; the page parses it to those five values and to `api` = api.beta-test.net with the clock at `ts`. Both repositories commit it.
- Reserved (not displayed in v1): check code = first 6 base32 (RFC 4648, A-Z2-7) characters of SHA-256 over the ASCII bytes of `state`; SHA-256('abc') gives XJ4BNP.

## 10. Out of scope for v1

Cross-device approval or polling; a loopback return (the documented fallback if the custom scheme proves unreliable, same gate); persisted attempts and cold-start sign-in; device label; languages other than English; platforms other than Windows; guest refusal.

## 11. Version 2: a request that names its network

Why. Version 1 means the official network: the page signs in on its default server, and the app offers the action only in the official network space. A person who points the app at another network (a beta or self-hosted deployment: Change Network API) has an account there and not on the default server. The page is a static app that can talk to any API that allows its origin, so the same page can approve for that network if the request says which API the app is on and the page checks that its browser is set to it.

Request. Version 2 is version 1 plus one key, `api`:

    <page URL>#v=2&platform=windows&state=<S>&pk=<PK>&ts=<T>&api=<A>

| Key | Value |
| --- | --- |
| `v` | `2` |
| `api` | the host of the app's API, `host` or `host:port`, `https` implied: ASCII lower-case letters, digits, hyphens and dots, never a scheme, path, userinfo or query. Labels of 1 to 63 characters that start and end with a letter or digit; at least two labels; the last label is neither all digits nor `0x` followed by hex digits (a browser reads either as a number, so no IP literal in any spelling); a port is 1 to 65535 without a leading zero and is never `443`; at most 200 characters in all. |

- A version 1 request carries no `api` and a version 2 request carries exactly one; either mismatch makes the whole request invalid. Every other rule of section 2 holds. The fragment alphabet gains `.` and `:`, and each value is still matched by its own pattern, so no other key can use them.
- The app sends version 2 only when its network space is not the official one, its API URL is `https://` plus a valid `api` value (no path, query or fragment; an `http` API, an IP literal or a non-ASCII host is not offered), and the descriptor lists 2. In the official network it sends version 1, whatever else the descriptor lists. The API URL is the one the client really uses, not the host name that was typed. It is read the way a URL is read, which is how the client and the page read it: capitals in the scheme or the host are the same host in small letters, an explicit `:443` is https' own port and names the same server without it, and a single trailing `/` is ignored; the `api` value is the small-letter form without `:443`. The official network is the operator's own space (its host, and no API or connect URL of the person's own) whose API really is the operator's `api.<host>`: a space with the operator's host but another API under it (a test environment of the operator, whose name the SDK puts in front of the service label) is named by that API like any other network, so that version 1, which means the page's default server, is never sent for an API that is not that one.
- The descriptor (section 1) is fetched once per run in every network whose API can be named, not only in the official one, because the version a network needs is listed in it. So an app on another network makes one request per run to the page's host for it, as it does in the official network; that request carries nothing of the network, and the app makes none when no network can be named.
- The attempt is bound to the network it started in: a network change ends it (section 5, rule 5), and the redeem goes to the API the app is on. The binding is checked once more where the code leaves the app, because an accepted answer can wait for a sheet in the way to close: if a network change (or any other sign-in flow) has begun since the attempt did, nothing is sent and the redeem is refused. The return (section 3) is unchanged: the code belongs to the API the page used, and the page used the one the request names.

Page. The request's `api` is compared with the host the browser's API server resolves to (`new URL(base, location.href).host`: a relative base resolves against the page, and a base that is not `https` never matches).

- Same host: the card works as in section 4 on that server, including when it is not the default.
- Different host, or a version 1 request and a browser set to a custom server: before any sign-in redirect and before any call to any API, the card is 'Different API server'. It names the hosts as plain text. Version 2 offers Cancel and 'Switch to {host}'; version 1 keeps its one action, 'Use the default server', which first signs this browser out of the server it leaves, then removes the override and goes on to the sign-in on the default server.
- Switch signs this browser out (the stored session belongs to the other server and is never sent to the new one, by this tab or by any other tab of the browser), points the browser at `https://{host}` (or back to the default when that is the default's host), and goes on to the sign-in and back to the card. It sits behind the sentence 'Only continue if you set this server in URnetwork yourself.'
- The request's host is text on the page and is not used as an address until the person clicks Switch, and then only if the browser's own URL parser gives it back exactly as written (`new URL('https://' + host).host` is `host`): the card shows the text of the request, and what is stored is what the browser reads. For a host it reads otherwise Switch does nothing, and Cancel still answers. The grammar keeps out the names that every browser reads as numbers; whether an engine accepts a given `xn--` label is the engine's call. No token, code or session goes to any server except the one the browser was already set to or the one Switch has just set.
- Approve itself is unchanged: identity check, mint, seal, return.

Timing. The page waits up to 30 s for each API call, because the API of some networks is slow; the request window of section 6 is unchanged.
