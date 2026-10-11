# The console's certificate

The console is plain HTTP by default. With this option on, a board with PSRAM
generates its own certificate at first boot, tells you its fingerprint, and
serves the console over TLS while it is on your own network ([#11]).

Off by default. Turn it on under `idf.py menuconfig` → **Observore** →
*Serve the console over HTTPS in uplink mode*.

It shipped in two halves, deliberately, in the shape the census and the mesh
both used. The parts that can be wrong quietly — a key generated badly, a
fingerprint matching nothing a browser shows, a certificate costing more heap
than the board has — went out first and were looked at on hardware before
anything depended on them. That was the right call: the first board to run the
generating half boot-looped, and the heap threshold the serving half inherited
turned out to be wrong by 1,513 bytes.

## What the threat actually is

The issue this came from said the detection log "can be read by anything in
range holding the network passphrase". That is not quite true, and the
difference decides what the certificate has to achieve.

On WPA2-PSK every station derives its own pairwise key from its own four-way
handshake. Another station that knows the passphrase **cannot** passively
decrypt your unicast traffic — it has to capture your handshake first, or
ARP-spoof its way into the path. Group-addressed traffic is readable by every
station, because the group key is shared, but the console's replies are
unicast.

Forcing a handshake to be re-captured means a deauthentication, and a flood of
those is something this device detects and scores at six.

So the attacker worth designing against is not a bystander within radio range.
It is an active attacker already on the network — which is exactly the case
that encryption without authentication does not cover.

## Why not just self-signed and click through the warning

Because the warning is the entire security of the connection, and a warning
that gets clicked through every visit has none left in it.

It is worse than neutral. The warning looks identical whether or not somebody
is intercepting, because a browser cannot tell a device's own certificate from
an attacker's. Training the owner of a counter-surveillance device to dismiss
it would remove the one signal that would have told them. That is a bad trade
wearing a padlock.

## What this project has that most devices do not

A screen. Five of the nine boards have one.

So the certificate is self-signed and generated on the device, and its
fingerprint is shown somewhere an attacker on the network cannot reach: on the
serial console at boot, and on the **system page** of a board that has a
screen — tap through the pages to the one headed `OBSERVORE system`:

```
 cert     185E 57A3 5E5A 67B9 38B9
```

That line was claimed by this document before it existed. The fingerprint was
printed on serial and nowhere else for two releases, while the argument for
this whole approach rested on the screen — and a display board is precisely
the case with no serial cable attached. It was caught by someone looking at
the board and not finding it.

```
W (3412) observore.httpscert: console certificate SHA-256 fingerprint:
  53:C7:0F:B9:4E:79:08:8E:C8:D2:3D:7E:A1:8F:59:E2:7E:1D:33:6C:77:95:1D:6D:9E:A7:BE:C9:9F:3A:52:88
W (3412) observore.httpscert: compare this with what the browser shows, once
```

Compare it once, the way an SSH host key is compared, and the warning stops
being a habit and becomes a check with a known answer.

This is trust on first use, and it is worth saying what that does not cover: an
attacker already in the path on the very first visit, before you have ever seen
the real fingerprint. Nothing here will tell you about that. What it does buy
is that any interception afterwards changes the fingerprint, while the device
goes on displaying the real one on hardware the attacker does not have.

A fingerprint is not a secret — it is public by construction — so unlike the
console password it is safe to put on a screen and safe to serve.

## Ten bytes on the glass, thirty-two everywhere else

A full SHA-256 is sixty-four hex characters. On a 320-pixel panel that is
either unreadable or four lines of hex nobody will actually compare, and a
comparison nobody performs is worth less than a shorter one they do.

The screen shows the first ten bytes, as five groups of four — twenty-four
characters, which fits beside a label on the narrowest panel here at forty
columns:

```
 cert     185E 57A3 5E5A 67B9 38B9
```

Ten bytes is eighty bits. To defeat that an attacker must produce a certificate
they hold the key to whose fingerprint shares those eighty bits — a
second-preimage search, not a birthday collision, because the target is a
fingerprint that already exists. That is 2^80 work aimed at reading one
household's detection log. The full thirty-two bytes are on the serial log for
anyone who would rather check all of it.

The format is the one browsers show, so the comparison is character for
character and nobody has to re-encode anything in their head.

## Why the hash is of the DER, not the PEM

Because that is what a browser calls the SHA-256 fingerprint. Hashing the PEM
gives a number that is correct, stable, reproducible, and matches nothing the
owner can see in their own browser — which makes it useless for the single
purpose it has.

This was checked rather than assumed. The same construction was compiled and
run on the host against the mbedTLS that ESP-IDF ships, and the result handed
to OpenSSL:

```
device  53:C7:0F:B9:4E:79:08:8E:C8:D2:3D:7E:A1:8F:59:E2:...
openssl 53:C7:0F:B9:4E:79:08:8E:C8:D2:3D:7E:A1:8F:59:E2:...
```

OpenSSL also confirms the certificate is self-signed and verifies against
itself, carries `CA:FALSE`, uses `prime256v1` with `ecdsa-with-SHA256`, and
that the generated private key belongs to it. A TLS 1.3 handshake against
`openssl s_server` completes with
`Verification error: self-signed certificate` — which is the expected result
and the warning the fingerprint exists to answer.

## What it actually cost, on a board

Measured on a Waveshare ESP32-S3 AMOLED, which has PSRAM, by capturing its
first boot after flashing:

```
I (834)  no certificate stored; generating one
I (1144) generation used 4668 of 12288 stack bytes, 7620 spare
I (1144) generated a P-256 certificate (635 bytes) and its key;
         internal heap 132367 -> 119223
W (1204) console certificate SHA-256 fingerprint:
         18:5E:57:A3:5E:5A:67:B9:38:B9:BE:B2:8D:7C:2E:52:...
```

| | |
|---|---|
| generation time | **310 ms**, once in the device's life |
| stack used | **4,668 bytes** |
| internal heap across generation | 132,367 → 119,223 |
| certificate | 635 bytes of PEM — the same figure the host produced |

A reboot loads it rather than making a new one, and reports the same
fingerprint — which is the property the whole scheme rests on, since a
fingerprint that changed on every boot would be one nobody could ever have
verified.

### It overflowed the main task first

The first board to run this printed
`A stack overflow in task main has been detected`, saved a core dump, and
boot-looped, rewriting that dump every 1.1 seconds.

Generation had been called straight from the init sequence. The main task has
6,144 bytes of stack in this build — a figure sized for something that calls
init functions, not something that generates a keypair and writes an X.509
certificate. The 4,668 bytes above needed to fit *on top of* whatever main was
already using, and did not.

It now runs on its own task, which exists only while generating and returns its
stack when it exits; raising the main task's stack would have spent that RAM
for the life of every device, on every board, for something that happens once.
Two 1,024- and 512-byte PEM buffers also moved off the stack into the statics
they were going to be copied into anyway.

Worth noting what reported it: the next boot said
`the previous run panicked in task "main" at pc 0x40383aa5` by itself, which is
the crash reporting from [firmware.md](firmware.md) doing its job on a bug
introduced two commits earlier.

### The threshold that was wrong, and what replaced it

The generating half carried a refusal threshold of 32 KB, described at the time
as chosen rather than measured. On hardware it refused to serve at **31,255
bytes free** — 1,513 short — on the one board class the option exists for.

What actually costs internal RAM is **one task stack**. Espressif's note that
"one socket uses ~40kB RAM with SSL" is about mbedTLS's session buffers, and
this project sets `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, so on a board with PSRAM
those come out of PSRAM. `httpd` runs a single task for every socket, so that
stack is paid once rather than per connection.

The handshake and the JSON handlers share it **sequentially**, so it needs the
larger of the two and not their sum. Getting that wrong would have cost 6 KB of
internal RAM on a board that serves with about 17 KB free.

So the floor is now derived from the stack rather than picked: `TLS_STACK` plus
the same 8 KB again.

## Serving it, measured

Run at a 12,288-byte stack first and asked what it used, then cut to 8,192 —
the same figure the plain server uses — and re-verified. Both numbers come off
the board, not off a spreadsheet:

| | 12,288-byte stack | **8,192 (shipped)** |
|---|---|---|
| internal heap to start the server | 16,872 | **12,764** |
| idle low-water, serving | 12,832 | **17,152** |
| low-water under authenticated load | 9,828 | **13,908** |
| httpd stack actually used | 3,312 | **3,304** |

The stack is not trimmed to the 3,304 measured. 8,192 leaves 59% headroom, and
what is unmeasured is named rather than assumed away: this was TLS 1.2 with one
self-signed P-256 certificate and no client certificate. A longer chain has not
been through it. Cutting to the first number that fitted is what
`observore_touch.c` carries a comment about.

### A handshake from another implementation

OpenSSL, over the network, to the board:

```
Protocol version: TLSv1.2
Ciphersuite: ECDHE-ECDSA-AES256-GCM-SHA384
Peer certificate: CN=observore-339464, O=Observore
Signature type: ecdsa_secp256r1_sha256
Verification error: self-signed certificate

sha256 Fingerprint=18:5E:57:A3:5E:5A:67:B9:38:B9:BE:B2:8D:7C:2E:52:...
```

**That fingerprint is the one the device printed on its own serial log, byte for
byte.** Which is the whole point: what the device shows you out of band is what
a client sees on the wire. The verification error is the expected result and the
warning the fingerprint answers.

TLS 1.2 rather than 1.3 because ESP-IDF does not enable
`MBEDTLS_SSL_PROTO_TLS1_3` by default; the suite negotiated is ECDHE with an
ECDSA signature and AES-256-GCM.

### It is not slow

The issue expected a three-second handshake to be a problem against a
thirty-second uplink window. Measured, a full request including the handshake:

| | |
|---|---|
| console page, 15,248 bytes | **0.77 s** |
| `/api/history`, 8,993 bytes | 0.62 s |
| `/api/census`, 5,692 bytes | 0.72 s |

Session tickets are on, which is why the option **selects**
`ESP_TLS_SERVER_SESSION_TICKETS` rather than leaving it to be remembered: asking
`esp_https_server` for tickets without that symbol makes `httpd_ssl_start` fail
outright with `ESP_ERR_NOT_SUPPORTED`, and the console then comes up plain
reporting a TLS failure whose cause is nowhere near the message. That cost an
hour of looking at heap figures that were fine.

### What happens when it cannot

Every refusal says which reason it was, because a console that silently came up
plain while the owner believed it was encrypted is worse than one that says it
could not:

```
serving plain HTTP: no certificate was generated
serving plain HTTP: 19104 bytes of internal heap free, below the 16384 this needs to serve TLS
could not start HTTPS (ESP_ERR_NOT_SUPPORTED); serving plain HTTP
```

And the address the device prints carries the scheme actually in use, never the
one that was intended:

```
console at https://172.25.42.89/ or https://observore.local/ on your network
```

Under thirty concurrent requests the log shows
`mbedtls_ssl_handshake returned -0x0050` a few times, which is
`MBEDTLS_ERR_NET_CONN_RESET`. That is this project's own tuning working as
designed rather than a TLS fault: `max_open_sockets` is 3 and `linger_timeout`
is 0, so the oldest connection is reset when a fourth arrives. One person
reading a page never sees it.

## Why P-256 and not RSA

Key generation happens on the device, once, at first boot. An RSA-2048 keypair
costs seconds to minutes on these parts and the time varies with luck, because
it is searching for primes. A P-256 key is one scalar multiplication. Browsers
have accepted ECDSA certificates for years.

The certificate comes out at 635 bytes of PEM and the key at 227.

## Validity is stated, not enforced

This device frequently has no idea what time it is — that has
[a section of its own](time.md) — so a certificate it generates at first boot
cannot have an honest `notBefore`.

A browser that checks the dates will complain, and it was going to complain
anyway about a self-signed certificate. The fingerprint is the thing being
verified, and expiry adds nothing to that, while a short window would mean the
fingerprint the owner wrote down stops working for no benefit. So the window is
2020 to 2040.

## Where this will not be offered

**Not on the SoftAP.** During first-time setup there is no name to put in a
certificate and no fingerprint anybody has seen yet, and WPA2 already protects
that link with a password only the owner has. The SoftAP stays HTTP
deliberately.

**Not where the internal heap cannot carry it.** Serving a handshake costs more
than making one, and the figures in [memory.md](memory.md) are not close:

| board class | min free internal heap under an outbound handshake |
|---|---|
| PSRAM (S3, C5 where fitted) | about **10,372 bytes** — mbedTLS allocates from PSRAM |
| XIAO C6 (headless, no PSRAM) | tight; keeps the default allocator |
| the three classic-ESP32 displays | **820 to 932 bytes** |

On 820 bytes that is not a tuning problem. So this is for the boards with
PSRAM, and the decision is made at runtime rather than by a compile-time list
of board names — the C5 profiles set `SPIRAM_IGNORE_NOTFOUND`, so whether a
given C5 has PSRAM is not knowable when the firmware is built.

The threshold that refusal uses is currently **chosen, not measured**: 32 KB,
being the 20 KB the outbound update path earned plus headroom. There is no
serving measurement to derive it from until something serves, and it wants
replacing with a real low-water mark then. Calling a chosen number measured is
how 15,360 bytes got quoted against the wrong heap pool for two releases.

When the heap is short the device will serve plain HTTP and say why. A console
that answers is worth more than one that was meant to be encrypted and is not
there at all.

[#11]: https://github.com/Forged-in-Feathers-Technology/observore/issues/11
