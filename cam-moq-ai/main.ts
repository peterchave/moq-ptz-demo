/**
 * moq2ai/main.ts — MoQT → YOLO object-detection pipeline
 *
 * Flow:
 *   1. Subscribe to a namespace prefix on the relay and wait for it to be
 *      announced (NAMESPACE message).
 *   2. Subscribe to the MSF catalog track inside that namespace.
 *   3. Parse the catalog, find the 1-fps keyframe video track.
 *   4. Subscribe to the keyframe track; for each frame:
 *        a. Decode the LOC payload to JPEG via ffmpeg.
 *        b. POST the JPEG to the YOLO detection API.
 *   5. Publish each YOLO result as a JSON object on a configurable
 *      output track (default: "objects").
 *
 * Environment variables:
 *   RELAY_URL           WebTransport relay URL          (default: https://localhost:4443)
 *   SUBSCRIBE_NAMESPACE Namespace to watch / subscribe  (default: live)
 *   PUBLISH_NAMESPACE   Namespace to publish on         (default: SUBSCRIBE_NAMESPACE)
 *   PUBLISH_TRACK       Output track name               (default: objects)
 *   YOLO_URL            YOLO predict endpoint           (default: http://YOLO_SERVER_IP:8000/predict)
 *   CERT_HASH           Hex SHA-256 of relay TLS cert   (optional, 64 hex chars)
 *   DRAFT_VERSION       MoQT draft version: 14|16|18    (default: 16)
 */

import { spawn } from 'node:child_process';
import { WebTransport as WT, quicheLoaded } from '@fails-components/webtransport';
import { MoqtConnection } from '@moqt/webtransport';
import { varint, SubgroupIdMode, PublishDoneCode } from '@moqt/transport';
import type { MoqtObject } from '@moqt/transport';
import { CATALOG_TRACK_NAME, parseCatalog, buildCatalog } from '@moqt/msf';
import type { Catalog, CatalogTrack } from '@moqt/msf';
import { parseLocHeaders } from '@moqt/loc';

// ─── Configuration ─────────────────────────────────────────────────

const RELAY_URL = process.env['RELAY_URL']            ?? 'https://localhost:4443';
const SUB_NS    = process.env['SUBSCRIBE_NAMESPACE']  ?? 'live';
const PUB_NS    = process.env['PUBLISH_NAMESPACE']    ?? SUB_NS;
const PUB_TRACK = process.env['PUBLISH_TRACK']        ?? 'objects';
const YOLO_URL  = process.env['YOLO_URL']             ?? 'http://YOLO_SERVER_IP:8000/predict';
const CERT_HASH = process.env['CERT_HASH'];
const DRAFT: 14 | 16 | 18 =
    process.env['DRAFT_VERSION'] === '14' ? 14 :
    process.env['DRAFT_VERSION'] === '18' ? 18 : 16;
/** Set ENABLE_PUBLISHER=0 to run the subscriber only (useful during testing). */
const ENABLE_PUBLISHER = process.env['ENABLE_PUBLISHER'] !== '0';

// ─── Types ──────────────────────────────────────────────────────────

interface YoloPrediction {
    class_id: number;
    label: string;
    confidence: number;
    bbox: [number, number, number, number];
}

interface YoloApiResponse {
    success: boolean;
    count: number;
    predictions: YoloPrediction[];
}

interface DetectionResult extends YoloApiResponse {
    /** Unix epoch milliseconds when the frame was processed. */
    timestamp: number;
    /** MoQT group ID of the source keyframe. */
    groupId: number;
}

// ─── Shared publish state ────────────────────────────────────────────

const enc = new TextEncoder();
const dec = new TextDecoder();

/** Publisher MoQT connection, set once the publisher session is up. */
let pubConn: MoqtConnection | null = null;
/** Track alias assigned by the relay when it subscribes to our objects track. */
let objsAlias: bigint | null = null;
/** Monotonically increasing group ID for published objects. */
let nextGroupId = 0n;
/** YOLO results buffered before the publisher is ready. */
const pendingResults: DetectionResult[] = [];

// ─── Helpers ─────────────────────────────────────────────────────────

function log(msg: string): void {
    console.log(`[${new Date().toISOString()}] ${msg}`);
}

/** Split a slash-delimited namespace string into a MoQT tuple. */
function toNsBytes(ns: string): Uint8Array[] {
    return ns.split('/').filter(s => s.length > 0).map(s => enc.encode(s));
}

/** Build WebTransport constructor options (protocol + optional cert pin). */
function wtOptions(): object {
    const opts: Record<string, unknown> = { protocols: [`moqt-${DRAFT}`] };
    if (CERT_HASH && CERT_HASH.length === 64) {
        const bytes = new Uint8Array(32);
        for (let i = 0; i < 32; i++) {
            bytes[i] = parseInt(CERT_HASH.slice(i * 2, i * 2 + 2), 16);
        }
        opts['serverCertificateHashes'] = [{ algorithm: 'sha-256', value: bytes }];
    }
    return opts;
}

// ─── Video frame → JPEG ──────────────────────────────────────────────

/** Detect Annex B start code at the beginning of a buffer. */
function isAnnexB(data: Uint8Array): boolean {
    return data.length >= 4
        && data[0] === 0 && data[1] === 0
        && (data[2] === 1 || (data[2] === 0 && data[3] === 1));
}

/**
 * Convert an H.264 AVCC-format frame (length-prefixed NALUs) to Annex B,
 * prepending SPS/PPS extracted from the AVCC decoder configuration record.
 */
function avccToAnnexB(init: Uint8Array, frame: Uint8Array): Uint8Array {
    const SC = new Uint8Array([0, 0, 0, 1]);
    const parts: Uint8Array[] = [];

    // byte 4 of the AVCDecoderConfigurationRecord: 0b111111 || lengthSizeMinusOne
    const nalLen = (init[4]! & 0x3) + 1;

    let p = 5;
    // SPS NAL units
    const numSPS = init[p++]! & 0x1F;
    for (let i = 0; i < numSPS; i++) {
        const l = (init[p]! << 8) | init[p + 1]!; p += 2;
        parts.push(SC, init.subarray(p, p + l)); p += l;
    }
    // PPS NAL units
    if (p < init.length) {
        const numPPS = init[p++]!;
        for (let i = 0; i < numPPS; i++) {
            const l = (init[p]! << 8) | init[p + 1]!; p += 2;
            parts.push(SC, init.subarray(p, p + l)); p += l;
        }
    }

    // Convert frame body: replace each length prefix with a start code
    let fp = 0;
    while (fp + nalLen <= frame.length) {
        let l = 0;
        for (let i = 0; i < nalLen; i++) l = (l << 8) | frame[fp++]!;
        if (l === 0 || fp + l > frame.length) break;
        parts.push(SC, frame.subarray(fp, fp + l)); fp += l;
    }

    return concatUint8(parts);
}

/**
 * Convert an H.265 HEVC-format frame to Annex B using the
 * HEVCDecoderConfigurationRecord from the catalog's initData.
 */
function hevcToAnnexB(init: Uint8Array, frame: Uint8Array): Uint8Array {
    const SC = new Uint8Array([0, 0, 0, 1]);
    const parts: Uint8Array[] = [];

    // HEVCDecoderConfigurationRecord layout:
    //   ...21 bytes of profile/level/tier info...
    //   byte 21: reserved(6) | lengthSizeMinusOne(2)
    //   byte 22: numOfArrays
    if (init.length < 23) return frame;
    const nalLen = (init[21]! & 0x3) + 1;
    const numArrays = init[22]!;

    let p = 23;
    for (let i = 0; i < numArrays && p < init.length; i++) {
        p++; // array_completeness + reserved + nal_unit_type
        if (p + 2 > init.length) break;
        const numNalus = (init[p]! << 8) | init[p + 1]!; p += 2;
        for (let j = 0; j < numNalus && p + 2 <= init.length; j++) {
            const l = (init[p]! << 8) | init[p + 1]!; p += 2;
            if (p + l > init.length) break;
            parts.push(SC, init.subarray(p, p + l)); p += l;
        }
    }

    // Convert frame body
    let fp = 0;
    while (fp + nalLen <= frame.length) {
        let l = 0;
        for (let i = 0; i < nalLen; i++) l = (l << 8) | frame[fp++]!;
        if (l === 0 || fp + l > frame.length) break;
        parts.push(SC, frame.subarray(fp, fp + l)); fp += l;
    }

    return concatUint8(parts);
}

function concatUint8(parts: Uint8Array[]): Uint8Array {
    const total = parts.reduce((a, b) => a + b.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const p of parts) { out.set(p, off); off += p.length; }
    return out;
}

/**
 * Decode a raw video payload to JPEG via ffmpeg.
 *
 * If `initData` is present and the payload is not already Annex B,
 * the payload is converted from AVCC / HEVC-length-prefixed to Annex B
 * before being piped to ffmpeg.
 */
async function frameToJpeg(
    codec: string,
    initData: Uint8Array | undefined,
    payload: Uint8Array,
): Promise<Buffer> {
    const isHevc = /^hvc|^hev/i.test(codec);
    let data: Uint8Array;

    if (initData && !isAnnexB(payload)) {
        // AVCC / length-prefixed format → convert to Annex B
        data = isHevc ? hevcToAnnexB(initData, payload) : avccToAnnexB(initData, payload);
    } else {
        data = payload;
    }

    const inputFormat = isHevc ? 'hevc' : 'h264';

    return new Promise<Buffer>((resolve, reject) => {
        const ff = spawn('ffmpeg', [
            '-loglevel', 'error',
            '-f', inputFormat,
            '-i', 'pipe:0',
            '-vframes', '1',
            '-f', 'image2pipe',
            '-vcodec', 'mjpeg',
            '-q:v', '3',
            'pipe:1',
        ], { stdio: ['pipe', 'pipe', 'pipe'] });

        const chunks: Buffer[] = [];
        ff.stdout.on('data', (c: Buffer) => chunks.push(c));
        ff.on('close', (code: number | null) => {
            if (code !== 0) reject(new Error(`ffmpeg exited with code ${code}`));
            else resolve(Buffer.concat(chunks));
        });
        ff.on('error', reject);

        ff.stdin.write(Buffer.from(data.buffer, data.byteOffset, data.byteLength));
        ff.stdin.end();
    });
}

// ─── YOLO API ────────────────────────────────────────────────────────

async function callYolo(jpeg: Buffer): Promise<YoloApiResponse> {
    const form = new FormData();
    // slice() returns a plain ArrayBuffer (not SharedArrayBuffer), satisfying Blob's type
    const ab = jpeg.buffer.slice(jpeg.byteOffset, jpeg.byteOffset + jpeg.byteLength) as ArrayBuffer;
    form.append('file', new Blob([ab], { type: 'image/jpeg' }), 'frame.jpg');
    const res = await fetch(YOLO_URL, {
        method: 'POST',
        headers: { accept: 'application/json' },
        body: form,
    });
    if (!res.ok) throw new Error(`YOLO API ${res.status}: ${res.statusText}`);
    return res.json() as Promise<YoloApiResponse>;
}

// ─── Publishing helpers ─────────────────────────────────────────────

/** Publish a single detection result as a JSON MoQT object. */
async function publishResult(result: DetectionResult): Promise<void> {
    if (!pubConn || objsAlias === null) {
        pendingResults.push(result);
        return;
    }
    const payload = enc.encode(JSON.stringify(result));
    const gid = nextGroupId++;
    const sid = await pubConn.openSubgroup(
        varint(objsAlias), varint(gid), varint(0n),
        { endOfGroup: true, defaultPriority: true, subgroupIdMode: SubgroupIdMode.ZERO },
    );
    await pubConn.sendObject(sid, varint(0n), payload);
    await pubConn.closeSubgroup(sid);
    log(`[pub] Published group=${gid} count=${result.count}`);
}

/** Drain any results that arrived before the publisher was ready. */
async function flushPending(): Promise<void> {
    while (pendingResults.length > 0 && pubConn && objsAlias !== null) {
        await publishResult(pendingResults.shift()!);
    }
}

/**
 * Respond to the relay's SUBSCRIBE for our catalog track:
 * send a single-object catalog then signal TRACK_ENDED.
 */
async function sendCatalog(conn: MoqtConnection, reqId: bigint, alias: bigint): Promise<void> {
    await conn.acceptSubscribe(varint(reqId), varint(alias));
    const payload = buildCatalog({
        tracks: [{
            name: PUB_TRACK,
            packaging: 'loc',
            isLive: true,
            role: 'ai-detections',
        }],
    });
    const gid = varint(BigInt(Date.now()));
    const sid = await conn.openSubgroup(
        varint(alias), gid, varint(0n),
        { endOfGroup: true, defaultPriority: true, subgroupIdMode: SubgroupIdMode.ZERO },
    );
    await conn.sendObject(sid, varint(0n), payload);
    await conn.closeSubgroup(sid);
    await conn.publishDone(varint(reqId), PublishDoneCode.TRACK_ENDED, '');
    log('[pub] Catalog sent to relay.');
}

// ─── Publisher flow ─────────────────────────────────────────────────

async function runPublisher(): Promise<void> {
    log(`[pub] Connecting to ${RELAY_URL} (publish ns: ${PUB_NS})...`);
    const transport = new WT(RELAY_URL, wtOptions() as any);
    await transport.ready;

    const conn = new MoqtConnection(DRAFT);
    pubConn = conn;
    conn.onMessage = (msg) => log(`[pub] ctrl: ${msg.type}`);
    conn.onClose   = (err, reason) => log(`[pub] Closed: ${err ?? 'ok'} ${reason ?? ''}`);
    conn.onError   = (err) => log(`[pub] Error: ${err.message}`);

    let nextAlias = 1n;
    conn.onSubscribe = (reqId, _ns, nameBytes) => {
        const name = dec.decode(nameBytes);
        const alias = nextAlias++;
        log(`[pub] Relay subscribed to '${name}' (alias=${alias})`);

        if (name === CATALOG_TRACK_NAME) {
            void sendCatalog(conn, reqId, alias)
                .catch(e => log(`[pub] Catalog error: ${(e as Error).message}`));
        } else if (name === PUB_TRACK) {
            objsAlias = alias;
            void conn.acceptSubscribe(varint(reqId), varint(alias))
                .then(() => {
                    log(`[pub] Objects track accepted. Flushing ${pendingResults.length} queued result(s)...`);
                    return flushPending();
                })
                .catch(e => log(`[pub] Accept error: ${(e as Error).message}`));
        } else {
            void conn.rejectSubscribe(reqId, 0n, `Unknown track: ${name}`);
        }
    };

    await conn.connect(transport as any, { maxRequestId: varint(64n) });
    await conn.publishNamespace(toNsBytes(PUB_NS));
    log(`[pub] Namespace '${PUB_NS}' announced. Waiting for relay subscriptions...`);
}

// ─── Subscriber flow ────────────────────────────────────────────────

/** Find the 1-fps keyframe track in the catalog. */
function findKeyframeTrack(catalog: Catalog): CatalogTrack | undefined {
    // Primary: video LOC track with framerate ≤ 1
    const byRate = catalog.tracks.find(
        t => t.packaging === 'loc'
          && (t.role === 'video' || t.role === undefined)
          && t.framerate !== undefined && t.framerate <= 1,
    );
    if (byRate) return byRate;

    // Secondary: any LOC track whose name contains "keyframe"
    const byName = catalog.tracks.find(
        t => t.packaging === 'loc' && /keyframe/i.test(t.name),
    );
    if (byName) return byName;

    // Fallback: any video LOC track — will process objectId=0 (IDR) frames only
    return catalog.tracks.find(
        t => t.packaging === 'loc' && t.role === 'video',
    );
}

async function runSubscriber(): Promise<void> {
    log(`[sub] Connecting to ${RELAY_URL} (subscribe ns: ${SUB_NS})...`);
    const transport = new WT(RELAY_URL, wtOptions() as any);
    await transport.ready;

    const conn = new MoqtConnection(DRAFT);
    conn.onMessage = (msg) => log(`[sub] ctrl: ${msg.type}`);
    conn.onClose   = (err, reason) => log(`[sub] Closed: ${err ?? 'ok'} ${reason ?? ''}`);
    conn.onError   = (err) => log(`[sub] Error: ${err.message}`);

    await conn.connect(transport as any, { maxRequestId: varint(100n) });

    const ns = toNsBytes(SUB_NS);

    // ── Step 1: SUBSCRIBE_NAMESPACE (fire-and-forget) ────────────────
    // We send SUBSCRIBE_NAMESPACE so the relay knows we are interested in
    // this namespace prefix and will route our track subscriptions to the
    // publisher.  Many relays do NOT forward NAMESPACE announcements back
    // to the subscriber, so we do NOT block waiting for a NAMESPACE msg —
    // we proceed directly to the catalog subscription.
    log(`[sub] Sending SUBSCRIBE_NAMESPACE '${SUB_NS}'...`);
    await conn.subscribeNamespace(ns);
    log('[sub] SUBSCRIBE_NAMESPACE sent. Proceeding to catalog...');

    // ── Step 2: FETCH catalog (group 0, object 0) ───────────────────
    // FETCH is finite (one object), unlike SUBSCRIBE which stays open.
    // Note: fetch objects always carry trackAlias = 0 (no alias on wire),
    // so we cannot filter by alias — just parse whatever arrives.
    let catResolve: ((c: Catalog) => void) | null = null;
    const catPromise = new Promise<Catalog>(r => { catResolve = r; });

    conn.onObject = (_streamId, obj) => {
        if (obj.kind !== 'data') return;
        try {
            // Some relays send "version":"1" (string) — coerce to number before parsing.
            const text = new TextDecoder().decode(obj.payload);
            const raw: Record<string, unknown> = JSON.parse(text);
            if (typeof raw['version'] === 'string') raw['version'] = Number(raw['version']);
            const cat = parseCatalog(JSON.stringify(raw), SUB_NS);
            catResolve?.(cat);
            catResolve = null;
        } catch (e) {
            log(`[sub] catalog parse error: ${(e as Error).message}`);
        }
    };

    const catReqId = await conn.fetch(ns, enc.encode(CATALOG_TRACK_NAME), {
        startGroup: varint(0n), startObject: varint(0n),
        endGroup:   varint(0n), endObject:   varint(0n),
    });
    log(`[sub] FETCH catalog sent (reqId=${catReqId}). Waiting for catalog object...`);
    const catalog = await catPromise;

    log(`[sub] Catalog received: ${catalog.tracks.length} track(s)`);
    for (const t of catalog.tracks) {
        log(`[sub]   ${t.name}  role=${t.role ?? '-'}  fps=${t.framerate ?? '-'}  codec=${t.codec ?? '-'}`);
    }

    // ── Step 3: Find the keyframe track ─────────────────────────────
    const kfTrack = findKeyframeTrack(catalog);
    if (!kfTrack) {
        log('[sub] ERROR: No keyframe track (1-fps LOC video) found in catalog. Exiting subscriber.');
        return;
    }
    log(`[sub] Keyframe track: '${kfTrack.name}'  fps=${kfTrack.framerate ?? '?'}  codec=${kfTrack.codec ?? '?'}`);

    const codec = kfTrack.codec ?? 'avc1';
    let initData: Uint8Array | undefined;
    if (kfTrack.initData) {
        initData = Buffer.from(kfTrack.initData, 'base64');
        log(`[sub] initData: ${initData.length} bytes (AVCC/HEVC decoder config record)`);
    }

    // ── Step 4: Subscribe to keyframe track; process each frame ─────
    let busy   = false;
    let frames = 0;

    const processFrame = async (obj: MoqtObject): Promise<void> => {
        // Narrow to MoqtObjectData — MoqtObjectGap has no payload
        if (obj.kind !== 'data') return;
        if (obj.payload.length === 0) return;

        // Check VideoFrameMarking — only process independent (keyframe) objects.
        // On a full-framerate track this skips delta frames; on a 1-fps keyframe
        // track every object is independent so nothing is dropped.
        const extBytes = obj.extensions ?? obj.properties;
        if (extBytes) {
            const hdr = parseLocHeaders(extBytes);
            if (hdr.videoFrameMarking && !hdr.videoFrameMarking.independent) return;
        } else if (obj.objectId !== 0n) {
            // No VideoFrameMarking extension: fall back to LOC §4.2 convention —
            // objectId=0 is the IDR (group-start keyframe), skip all others.
            return;
        }

        // Rate-limit: skip this frame if the previous one is still in flight.
        // On a 1-fps track this should rarely happen, but guards against slow
        // YOLO responses stacking up.
        if (busy) {
            log('[sub] Previous frame still processing — skipping.');
            return;
        }
        busy = true;
        frames++;

        log(`[sub] Frame #${frames}: group=${obj.groupId} objId=${obj.objectId} size=${obj.payload.length}B`);
        try {
            const jpeg = await frameToJpeg(codec, initData, obj.payload);
            log(`[sub] Decoded to JPEG (${jpeg.length}B) — calling YOLO...`);

            const yolo = await callYolo(jpeg);
            log(`[sub] YOLO result: success=${yolo.success} count=${yolo.count}`);

            const result: DetectionResult = {
                ...yolo,
                timestamp: Date.now(),
                groupId: Number(obj.groupId),
            };
            await publishResult(result);
        } catch (e) {
            log(`[sub] Frame processing error: ${(e as Error).message}`);
        } finally {
            busy = false;
        }
    };

    const kfSub = await conn.subscribeTrack(ns, enc.encode(kfTrack.name), {
        onObject(obj: MoqtObject) { void processFrame(obj); },
    });
    log(`[sub] Keyframe track subscribed (reqId=${kfSub.requestId}). Processing frames...`);
}

// ─── Entry point ─────────────────────────────────────────────────────

async function main(): Promise<void> {
    log('moq2ai starting');
    log(`  RELAY_URL:           ${RELAY_URL}`);
    log(`  SUBSCRIBE_NAMESPACE: ${SUB_NS}`);
    log(`  PUBLISH_NAMESPACE:   ${PUB_NS}`);
    log(`  PUBLISH_TRACK:       ${PUB_TRACK}`);
    log(`  YOLO_URL:            ${YOLO_URL}`);
    log(`  DRAFT_VERSION:       ${DRAFT}`);
    log('');

    log('Initialising libquiche (WebTransport native layer)...');
    await quicheLoaded;
    log('libquiche ready.\n');

    // Run subscriber and publisher concurrently; errors in one do not stop the other.
    const tasks: Promise<void>[] = [
        runSubscriber().catch(e => {
            log(`[sub] Fatal error: ${(e as Error).message}`);
            console.error(e);
        }),
    ];
    if (ENABLE_PUBLISHER) {
        tasks.push(
            runPublisher().catch(e => {
                log(`[pub] Fatal error: ${(e as Error).message}`);
                console.error(e);
            }),
        );
    } else {
        log('Publisher disabled (ENABLE_PUBLISHER=0).');
    }
    await Promise.all(tasks);
}

main().catch(e => {
    console.error('Fatal:', e);
    process.exit(1);
});
