/**
 * A/V Playback example — video + audio on screen.
 *
 * Full vertical slice exercising every layer:
 *   WebTransport → MoqtConnection → LOC headers → WebCodecs → Canvas + AudioContext
 *
 * Includes minimum gap handling required for live streams:
 * - Object Status 0x3 (End of Group) → skip forward
 * - Object Status 0x4 (End of Track) → stop
 * - Stream reset (DELIVERY_TIMEOUT) → wait for next keyframe
 * - Keyframe gating after gaps (delta frames without keyframe corrupt decoder)
 *
 * Audio requires a user gesture to start (Chrome autoplay policy).
 *
 * Uses the lower packages directly — not @moqt/player.
 *
 * @see draft-ietf-moq-transport-16 §3 (Session)
 * @see draft-ietf-moq-transport-16 §9.9 (SUBSCRIBE)
 * @see draft-ietf-moq-transport-16 §10.2.1.1 (Object Status)
 * @see draft-ietf-moq-transport-16 §9.2.2.2 (DELIVERY_TIMEOUT)
 * @see draft-ietf-moq-loc-01 §2.2 (LOC payload = EncodedVideoChunk/EncodedAudioChunk.data)
 * @see draft-ietf-moq-loc-01 §2.3.2.2 (VideoFrameMarking)
 * @see draft-ietf-moq-loc-01 §4.1 (Audio: each object independently decodable)
 * @see draft-ietf-moq-loc-01 §4.2 (Video: Group boundary = IDR boundary)
 * @see draft-ietf-moq-msf-00 §5.1.24 (Codec string)
 */

import { MoqtConnection } from '@moqt/webtransport';
import { varint, SubgroupIdMode, PublishDoneCode } from '@moqt/transport';
import type { MoqtObject, Varint } from '@moqt/transport';
import { CATALOG_TRACK_NAME, parseCatalog, buildCatalog } from '@moqt/msf';
import type { Catalog, CatalogTrack } from '@moqt/msf';
import { parseLocHeaders, toVideoChunkInit } from '@moqt/loc';
import { log } from './shared/log.js';
import { relayUrl, namespace, certHash } from './shared/cert.js';

// ─── Capability checks ──────────────────────────────────────────────

if (!('WebTransport' in window)) {
    log('WebTransport is not available. Chrome 97+ or Edge 97+ required.');
    throw new Error('WebTransport not supported');
}

if (!('VideoDecoder' in window)) {
    log('WebCodecs VideoDecoder is not available. Chrome 94+ required.');
    throw new Error('WebCodecs not supported');
}

// ─── Video State ─────────────────────────────────────────────────────

let videoTrackAlias: bigint | null = null;
let videoDecoder: VideoDecoder | null = null;
let videoConfigured = false;
let needsKeyframe = true; // Start true — need first keyframe before decoding
let frameCount = 0;
let videoCodec = '';
let videoWidth = 0;
let videoHeight = 0;
let videoInitData: Uint8Array | undefined;

// ─── Audio State ─────────────────────────────────────────────────────

let audioTrackAlias: bigint | null = null;
let audioDecoder: AudioDecoder | null = null;
let audioCtx: AudioContext | null = null;
let audioCodec = '';
let audioSampleRate = 0;
let audioChannels = 0;
let audioSampleCount = 0;  // Successfully decoded + played
let audioSubmitCount = 0;  // Submitted to decoder (for timestamp fallback)
let audioErrorCount = 0;
let firstAudioTimestamp: number | null = null;
let lastAudioFrameInfo = ''; // For error diagnostics
let nextAudioPlayTime = 0; // AudioContext.currentTime for seamless scheduling

// ─── PTZ Control State ──────────────────────────────────────────────

type PtzCommand = {
    type: 'ptz';
    action: 'move' | 'preset';
    axis?: 'pan' | 'tilt' | 'zoom';
    value?: number;
    /** Duration the camera moves before auto-stopping (ms). Bounded server-side too. */
    duration_ms?: number;
    preset_id?: number;
    seq: number;
    ts: number;
};

type ControlMessage = {
    type: string;
    requestId?: bigint;
};

const controlTrackAlias = 90n;
let controlConnection: MoqtConnection | null = null;
let controlPublishReady: Promise<void> | null = null;
let nextControlGroupId = 0n;
let nextControlSequence = 0;
let ptzControlsBound = false;
let ptzKeepAliveTimer: number | null = null;
/** Duration (ms) each atomic move lasts before the camera auto-stops. */
const PTZ_DURATION_MS = 300;
const PTZ_KEEPALIVE_MS = 10_000;

// ─── DOM ─────────────────────────────────────────────────────────────

const canvas = document.getElementById('canvas') as HTMLCanvasElement;
const ctx = canvas.getContext('2d')!;
const statsEl = document.getElementById('stats')!;
const startBtn = document.getElementById('start') as HTMLButtonElement;
const startControlsBtn = document.getElementById('start-controls') as HTMLButtonElement;
const ptzButtons = document.querySelectorAll<HTMLButtonElement>('[data-ptz-action]');


// ─── Helpers ─────────────────────────────────────────────────────────

const enc = new TextEncoder();

function createTransportOptions(): WebTransportOptions {
    const transportOptions: WebTransportOptions = {};
    transportOptions.protocols = ['moqt-16'];
    if (certHash) {
        transportOptions.serverCertificateHashes = [{
            algorithm: 'sha-256',
            value: certHash,
        }];
    }
    return transportOptions;
}

async function waitForPublishOk(
    connection: MoqtConnection,
    requestId: bigint,
    timeoutMs = 10_000,
): Promise<void> {
    return new Promise<void>((resolve, reject) => {
        const previous = connection.onMessage;
        const restore = (): void => {
            connection.onMessage = previous ?? (() => undefined);
        };
        const timer = window.setTimeout(() => {
            restore();
            reject(new Error(`Request ${requestId} was not accepted in time`));
        }, timeoutMs);

        connection.onMessage = (msg: ControlMessage) => {
            previous?.(msg);
            if (msg.type !== 'PUBLISH_OK' && msg.type !== 'REQUEST_OK') return;
            if (msg.requestId !== requestId) return;

            window.clearTimeout(timer);
            restore();
            resolve();
        };
    });
}

function encodeNamespace(ns: string): Uint8Array[] {
    return ns
        .split('/')
        .map((segment) => segment.trim())
        .filter((segment) => segment.length > 0)
        .map((segment) => enc.encode(segment));
}

function updateStats(): void {
    statsEl.textContent = `Video: ${frameCount} frames | Audio: ${audioSampleCount} chunks`;
}

async function sendControlObject(payload: Uint8Array): Promise<void> {
    if (!controlConnection || !controlPublishReady) {
        throw new Error('PTZ control connection is not ready');
    }

    await controlPublishReady;

    const cmdAlias: bigint = (controlConnection as any).__cmdAlias ?? controlTrackAlias;
    const groupId = nextControlGroupId++;
    const streamId = await controlConnection.openSubgroup(varint(cmdAlias), varint(groupId), varint(0n), {
        publisherPriority: 32,
        endOfGroup: true,
    });

    try {
        await controlConnection.sendObject(streamId, varint(0n), payload);
    } finally {
        await controlConnection.closeSubgroup(streamId);
    }
}

function stopPtzKeepAlive(): void {
    if (ptzKeepAliveTimer === null) return;
    window.clearInterval(ptzKeepAliveTimer);
    ptzKeepAliveTimer = null;
}

function startPtzKeepAlive(): void {
    if (ptzKeepAliveTimer !== null) return;
    ptzKeepAliveTimer = window.setInterval(() => {
        const payload = enc.encode(JSON.stringify({
            type: 'keepalive',
            ts: Date.now(),
        }));

        void sendControlObject(payload).catch((error) => {
            stopPtzKeepAlive();
            log(`PTZ keep-alive stopped: ${(error as Error).message}`);
        });
    }, PTZ_KEEPALIVE_MS);
}

async function publishControlCatalog(
    connection: MoqtConnection,
    requestId: bigint,
    alias: bigint,
): Promise<void> {
    await connection.acceptSubscribe(varint(requestId), varint(alias));

    const catalogPayload = buildCatalog({
        tracks: [{
            name: 'command',
            packaging: 'loc',
            isLive: true,
            role: 'data' as any,
        }],
    });

    const catalogGroupId = varint(BigInt(Date.now()));
    const streamId = await connection.openSubgroup(
        varint(alias), catalogGroupId, varint(0),
        {
            hasExtensions: false,
            endOfGroup: true,
            defaultPriority: true,
            subgroupIdMode: SubgroupIdMode.ZERO,
        },
    );
    await connection.sendObject(streamId, varint(0), catalogPayload);
    await connection.closeSubgroup(streamId);
    await connection.publishDone(varint(requestId), PublishDoneCode.TRACK_ENDED, '');
    log('PTZ catalog published.');
}

async function publishControlTrack(
    controlNamespace: string,
): Promise<void> {
    if (controlPublishReady) {
        await controlPublishReady;
        return;
    }

    const publishReady = (async () => {
        log('12. Connecting PTZ control session...');
        const transport = new WebTransport(relayUrl, createTransportOptions());
        await transport.ready;

        const connection = new MoqtConnection(16);
        connection.onMessage = (msg: ControlMessage) => {
            log(`PTZ control: ${msg.type}`);
        };
        connection.onClose = (error: unknown, reason: unknown) => {
            stopPtzKeepAlive();
            log(`PTZ session closed: error=${error ?? 'none'} reason=${reason ?? ''}`);
        };
        connection.onError = (error: Error) => {
            log(`PTZ session error: ${error.message}`);
        };

        // Handle relay subscribing to catalog and command tracks
        let nextAlias = 1n;
        connection.onSubscribe = (reqId: bigint, _ns: unknown, trackNameBytes: Uint8Array) => {
            const name = new TextDecoder().decode(trackNameBytes);
            const alias = nextAlias++;
            log(`PTZ relay subscribed to '${name}' (alias=${alias})`);
            if (name === 'catalog') {
                void publishControlCatalog(connection, reqId, alias).catch((e) =>
                    log(`PTZ catalog publish error: ${(e as Error).message}`));
            } else if (name === 'command') {
                // Remember alias so sendPtzCommand can open subgroups on it
                (connection as any).__cmdAlias = alias;
                void connection.acceptSubscribe(varint(reqId), varint(alias));
                log('PTZ command track accepted, ready to send commands.');
                startPtzKeepAlive();
            } else {
                void connection.rejectSubscribe(varint(reqId), varint(0), `Unknown track: ${name}`);
            }
        };

        await connection.connect(transport, {
            maxRequestId: varint(32),
        });
        controlConnection = connection;

        log('13. Announcing PTZ control namespace: ' + controlNamespace);
        const nsBytes = encodeNamespace(controlNamespace);
        await connection.publishNamespace(nsBytes);
        log('    Namespace announced — waiting for relay to subscribe...');
    })();

    controlPublishReady = publishReady;
    try {
        await publishReady;
    } catch (error) {
        stopPtzKeepAlive();
        controlConnection = null;
        controlPublishReady = null;
        throw error;
    }
}

async function sendPtzCommand(command: Omit<PtzCommand, 'type' | 'seq' | 'ts'>): Promise<void> {
    if (!controlConnection || !controlPublishReady) {
        log('PTZ control track is not ready yet.');
        return;
    }

    await controlPublishReady;

    const message: PtzCommand = {
        type: 'ptz',
        seq: nextControlSequence++,
        ts: Date.now(),
        ...command,
    };
    const payload = enc.encode(JSON.stringify(message));
    await sendControlObject(payload);
    if (message.action === 'preset') {
        log(`PTZ sent: preset ${message.preset_id} seq=${message.seq}`);
    } else {
        log(`PTZ sent: move ${message.axis}=${message.value} dur=${message.duration_ms}ms seq=${message.seq}`);
    }
}

function bindPtzControls(): void {
    if (ptzControlsBound) return;
    ptzControlsBound = true;

    const commandFromButton = (
        button: HTMLButtonElement,
    ): Omit<PtzCommand, 'type' | 'seq' | 'ts'> | null => {
        const action = button.dataset.ptzAction;
        if (action === 'move') {
            const axis = button.dataset.ptzAxis as 'pan' | 'tilt' | 'zoom' | undefined;
            const value = button.dataset.ptzValue;
            if (!axis || value === undefined) return null;
            return {
                action: 'move',
                axis,
                value: Number(value),
                duration_ms: PTZ_DURATION_MS,
            };
        }
        if (action === 'preset') {
            const preset = button.dataset.ptzPreset;
            if (preset === undefined) return null;
            return {
                action: 'preset',
                preset_id: Number(preset),
            };
        }
        return null;
    };

    for (const button of ptzButtons) {
        button.addEventListener('click', () => {
            const command = commandFromButton(button);
            if (!command) return;
            button.blur();
            void sendPtzCommand(command).catch((error) => {
                log(`PTZ send failed: ${(error as Error).message}`);
            });
        });
    }

    log(`PTZ onscreen controls ready (atomic ${PTZ_DURATION_MS}ms moves + presets).`);
}

/**
 * Prepend a 7-byte ADTS header to a raw AAC access unit.
 *
 * The W3C AAC WebCodecs Registration defines two modes:
 *   - description present  → raw `raw_data_block()` per ISO 14496-3 §4.4.2.1
 *   - description absent   → ADTS frames (sync word 0xFFF)
 *
 * We use ADTS mode because Chrome's platform decoders (AudioToolbox on macOS,
 * Media Foundation on Windows) are more reliable with ADTS-framed data than
 * with bare raw_data_block() syntax.
 *
 * @see ISO/IEC 14496-3 §1.A.3.1 (ADTS fixed header)
 * @see W3C AAC WebCodecs Registration §2 (ADTS EncodedAudioChunk format)
 */
function wrapInADTS(
    rawFrame: Uint8Array,
    sampleRate: number,
    channels: number,
): Uint8Array {
    const freqIndexMap: Record<number, number> = {
        96000: 0, 88200: 1, 64000: 2, 48000: 3, 44100: 4, 32000: 5,
        24000: 6, 22050: 7, 16000: 8, 12000: 9, 11025: 10, 8000: 11,
    };
    const freqIndex = freqIndexMap[sampleRate] ?? 3; // default 48kHz

    const frameLen = rawFrame.byteLength + 7; // 7 = ADTS header (no CRC)
    const header = new Uint8Array(7);

    // Byte 0-1: Sync word (0xFFF), MPEG-4 (ID=0), Layer=0, no CRC
    header[0] = 0xFF;
    header[1] = 0xF1;
    // Byte 2: Profile (AAC-LC=1, i.e. objectType-1), freq index, private, channel config MSB
    header[2] = (1 << 6) | (freqIndex << 2) | (channels >> 2);
    // Byte 3: Channel config LSBs, frame length upper 2 bits
    header[3] = ((channels & 0x3) << 6) | ((frameLen >> 11) & 0x3);
    // Byte 4: Frame length mid 8 bits
    header[4] = (frameLen >> 3) & 0xFF;
    // Byte 5: Frame length lower 3 bits, buffer fullness upper 5 bits (0x7FF = VBR)
    header[5] = ((frameLen & 0x7) << 5) | 0x1F;
    // Byte 6: Buffer fullness lower 6 bits, number of raw_data_blocks - 1 = 0
    header[6] = 0xFC;

    const adtsFrame = new Uint8Array(frameLen);
    adtsFrame.set(header);
    adtsFrame.set(rawFrame, 7);
    return adtsFrame;
}

/**
 * Create (or recreate) the AudioDecoder with error recovery.
 *
 * WebCodecs decoders enter 'closed' state on error and cannot be reused.
 * Since each AAC frame is independently decodable (LOC §4.1), we can
 * recreate the decoder and continue from the next frame.
 */
function createAudioDecoder(config: AudioDecoderConfig): void {
    audioDecoder = new AudioDecoder({
        output: (audioData: AudioData) => {
            // Play through AudioContext by copying decoded PCM into an AudioBuffer.
            // AudioData holds native memory — close() is required just like VideoFrame.
            const buf = audioCtx!.createBuffer(
                audioData.numberOfChannels,
                audioData.numberOfFrames,
                audioData.sampleRate,
            );

            for (let ch = 0; ch < audioData.numberOfChannels; ch++) {
                const dest = buf.getChannelData(ch);
                audioData.copyTo(dest, { planeIndex: ch, format: 'f32-planar' });
            }
            audioData.close();

            // Schedule seamless back-to-back playback.
            // If we've fallen behind (network stall, decoder pause), skip ahead to now.
            const now = audioCtx!.currentTime;
            if (nextAudioPlayTime < now) {
                nextAudioPlayTime = now;
            }

            const source = audioCtx!.createBufferSource();
            source.buffer = buf;
            source.connect(audioCtx!.destination);
            source.start(nextAudioPlayTime);
            nextAudioPlayTime += buf.duration;

            audioSampleCount++;
            if (audioSampleCount === 1) {
                log('First audio chunk played!');
            }
        },
        error: (err: DOMException) => {
            audioErrorCount++;
            if (audioErrorCount <= 5) {
                log(`Audio decode error #${audioErrorCount} at frame ~${audioSampleCount}: ${err.message} [${lastAudioFrameInfo}]`);
            }
            // Decoder is now 'closed' — recreate to continue decoding.
            createAudioDecoder(config);
        },
    });

    audioDecoder.configure(config);
    if (audioErrorCount === 0) {
        log(`Audio decoder configured: ${config.codec} ${config.sampleRate}Hz ${config.numberOfChannels}ch (adts mode)`);
    }
}

// ─── Main ────────────────────────────────────────────────────────────

// AudioContext requires a user gesture (Chrome autoplay policy).
// We gate the entire flow on the Start button click.
startBtn.addEventListener('click', () => {
    startBtn.disabled = true;
    startBtn.textContent = 'Starting...';
    audioCtx = new AudioContext();
    log(`AudioContext created (sampleRate=${audioCtx.sampleRate}).`);
    main().catch((err) => {
        log(`Fatal: ${(err as Error).message}`);
        console.error(err);
    });
});
// PTZ controls MOQT channel
startControlsBtn.addEventListener('click', () => {
    startControlsBtn.disabled = true;
    startControlsBtn.textContent = 'Starting...';

    // control namespace
    const controlNamespace = namespace+"/control"
    /* PTX controls */
    bindPtzControls();
    void publishControlTrack(controlNamespace).catch((err) => {
        log(`PTZ disabled: ${(err as Error).message}`);
        console.error(err);
    });
});

async function main(): Promise<void> {

    // video live namespace
    const liveNamespace = namespace+"/live"

    // control namespace
    const controlNamespace = namespace+"/control"

    log(`Relay: ${relayUrl}`);
    log(`Live namespace: ${liveNamespace}`);
    log(`Control namespace: ${controlNamespace}`);
    log('');

    // ── 1. WebTransport connection ──────────────────────────────────
    // @see draft-ietf-moq-transport-16 §3.1
    log('1: Creating WebTransport connection...');
    const transportOptions = createTransportOptions();
    const connectUrl = `${relayUrl}` ///?ns=${encodeURIComponent(liveNamespace)}`;
    const transport = new WebTransport(connectUrl, transportOptions);
    await transport.ready;
    log(`   WT protocol: "${transport.protocol}"`);
    log('   WebTransport connected.');

    // ── 2. MoqtConnection ─────────────────────────────────────────────
    // @see draft-ietf-moq-transport-16 §3
    log('2: Create MoqtConnection...');
    const connection = new MoqtConnection();

    // ── 3. Wire callbacks ──────────────────────────────────────────
    log('3: Wire callbacks...');

    connection.onMessage = (msg) => {
        log(`Control: ${msg.type}`);
    };

    connection.onClose = (error, reason) => {
        log(`Session closed: error=${error ?? 'none'} reason=${reason ?? ''}`);
    };

    connection.onError = (error) => {
        log(`Session error: ${error.message}`);
    };

    // Stream reset handling — DELIVERY_TIMEOUT resets mean objects on that
    // stream are lost; we need the next keyframe to resume decoding.
    // @see draft-ietf-moq-transport-16 §9.2.2.2 (DELIVERY_TIMEOUT)
    // @see draft-ietf-moq-transport-16 §13.4.4 (error code 0x2)
    connection.onStreamClosed = (_streamId, error) => {
        if (error !== undefined) {
            log(`Stream reset: error=0x${error.toString(16)}`);
            needsKeyframe = true;
        }
    };

    // Object handler — routes catalog, video, and audio objects.
    let catalogResolved: ((catalog: Catalog) => void) | null = null;
    const catalogPromise = new Promise<Catalog>((resolve) => {
        catalogResolved = resolve;
    });

    connection.onObject = (_streamId, obj) => {
        // Route by track alias
        if (videoTrackAlias !== null && BigInt(obj.trackAlias) === BigInt(videoTrackAlias)) {
            handleVideoObject(obj);
            return;
        }
        if (audioTrackAlias !== null && BigInt(obj.trackAlias) === BigInt(audioTrackAlias)) {
            handleAudioObject(obj);
            return;
        }

        // Assume anything else before media subscriptions is catalog
        if (obj.kind === 'gap') return;
        try {
            const catalog = parseCatalog(obj.payload!, liveNamespace);
            catalogResolved?.(catalog);
            catalogResolved = null;
        } catch {
            // Not a valid catalog — ignore
        }
    };

    // ── 4. Connect ─────────────────────────────────────────────────
    // maxRequestId MUST be >= 1 for subscriptions to work.
    // @see draft-ietf-moq-transport-16 §9.3.1.3
    log('4: Connecting to MOQT session...');
    await connection.connect(transport, {
        maxRequestId: varint(100),
    });
    log(`   Session established (state: ${connection.session.state}).`);


    /* SUBSCRIBE OR FETCH CATALOG */

    /*
    // ── 5a. Subscribe to catalog ────────────────────────────────────
    // @see draft-ietf-moq-transport-16 §9.9
    // @see draft-ietf-moq-msf-00 §5
    log('5: Subscribing to catalog...');
    await connection.subscribe(
        encodeNamespace(liveNamespace),
        enc.encode('catalog'),
    );
    log('   Waiting for catalog...');
    */

    // ── 5b. Fetch catalog ────────────────────────────────────────────
    // subscribe() is open-ended live delivery; fetch() is finite range retrieval.
    // We use fetch() here to get exactly one catalog object (group 0, object 0).
    // @see draft-ietf-moq-transport-16 §9.10
    // @see draft-ietf-moq-msf-00 §5
    log('5: Fetching catalog...');
    const nsBytes = encodeNamespace(liveNamespace);
    const catalogReqId = await connection.fetch(nsBytes, enc.encode(CATALOG_TRACK_NAME), {
        startGroup: varint(0n), startObject: varint(0n),
        endGroup: varint(0n), endObject: varint(0n),
    });
    log(`   FETCH catalog: reqId=${catalogReqId} group=0 object=0`);
    log('   Waiting for catalog...');

    /* END SUBSCRIBE OR FETCH CATALOG */

    const catalog = await catalogPromise;
    log(`   Catalog: version=${catalog.version}, ${catalog.tracks.length} tracks`);
    for (const t of catalog.tracks) {
        const parts: string[] = [t.name];
        if (t.codec) parts.push(t.codec);
        if (t.width && t.height) parts.push(`${t.width}x${t.height}`);
        if (t.bitrate) parts.push(`${(t.bitrate / 1000).toFixed(0)}kbps`);
        log(`     ${parts.join(' | ')}`);
    }

    // ── 6. Find video track ────────────────────────────────────────
    // @see draft-ietf-moq-msf-00 §5.1.12 (packaging must be "loc")
    // @see draft-ietf-moq-msf-00 §5.1.24 (codec)

    log('6: Finding video track...');

    const videoTrack: CatalogTrack | undefined = catalog.tracks.find(
        (t) => t.role === 'video' && t.packaging === 'loc',
    );
    if (!videoTrack) {
        log('   No LOC video track found in catalog.');
        return;
    }
    if (!videoTrack.codec) {
        log(`Video track "${videoTrack.name}" has no codec field.`);
        return;
    }

    videoCodec = videoTrack.codec;
    videoWidth = videoTrack.width ?? 1920;
    videoHeight = videoTrack.height ?? 1080;

    // Decode initData (Base64) if present in catalog.
    // @see draft-ietf-moq-msf-00 §5.1.20 (Initialization data)
    // @see draft-ietf-moq-loc-01 §2.1.2 (maps to VideoDecoderConfig.description)
    if (videoTrack.initData) {
        const binary = atob(videoTrack.initData);
        videoInitData = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) {
            videoInitData[i] = binary.charCodeAt(i);
        }
    }

    // Size canvas to video dimensions
    canvas.width = videoWidth;
    canvas.height = videoHeight;

    log(`   Video: ${videoTrack.name} | ${videoCodec} | ${videoWidth}x${videoHeight}`);

    // ── 7. Find audio track ────────────────────────────────────────
    // @see draft-ietf-moq-msf-00 §5.1.12 (packaging)
    // @see draft-ietf-moq-msf-00 §5.1.24 (codec)

    log('7: Finding audio track...');

    const audioTrack: CatalogTrack | undefined = catalog.tracks.find(
        (t) => t.role === 'audio' && t.packaging === 'loc',
    );

    if (audioTrack && audioTrack.codec) {
        audioCodec = audioTrack.codec;
        audioSampleRate = audioTrack.samplerate ?? 48000;
        audioChannels = Number(audioTrack.channelConfig ?? '2');
        log(`   Audio: ${audioTrack.name} | ${audioCodec} | ${audioSampleRate}Hz | ${audioChannels}ch`);
    } else {
        log('   No LOC audio track found in catalog (video-only mode).');
    }

    // ── 8. Create VideoDecoder ─────────────────────────────────────
    log('8: Creating VideoDecoder...');

    videoDecoder = new VideoDecoder({
        output: (frame: VideoFrame) => {
            // Render to canvas
            ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);

            // frame.close() is NON-NEGOTIABLE — VideoFrame holds GPU memory
            // outside JavaScript GC. 1 sec of 1080p25 ~ 200MB unclosed.
            frame.close();

            frameCount++;
            if (frameCount === 1) {
                log('   First video frame rendered!');
            }
            if (frameCount % 30 === 0) {
                updateStats();
            }
        },
        error: (err: DOMException) => {
            log(`   Video decoder error: ${err.message}`);
        },
    });

    // ── 9. Create AudioDecoder ─────────────────────────────────────
    // @see draft-ietf-moq-loc-01 §4.1 (Audio: each object independently decodable)
    log('9: Creating AudioDecoder...');

    if (audioTrack && audioTrack.codec && audioCtx) {
        // Configure audio decoder in ADTS mode — omit description so the decoder
        // expects ADTS-framed input. We prepend ADTS headers client-side in
        // handleAudioObject(). This is more reliable across Chrome's platform
        // decoders (AudioToolbox/macOS, Media Foundation/Windows) than raw mode.
        // @see W3C AAC WebCodecs Registration §2 (ADTS mode = no description)
        // @see draft-ietf-moq-msf-00 §5.1.24 (codec string from catalog)
        const audioConfig: AudioDecoderConfig = {
            codec: audioCodec,
            sampleRate: audioSampleRate,
            numberOfChannels: audioChannels,
        };

        const support = await AudioDecoder.isConfigSupported(audioConfig);
        log(`   Audio isConfigSupported: ${support.supported} (codec=${audioConfig.codec}, mode=adts)`);

        if (!support.supported) {
            log('Audio config not supported — skipping audio.');
        } else {
            createAudioDecoder(audioConfig);
        }
    }

    // ── 10. Subscribe to video track ───────────────────────────────
    // @see draft-ietf-moq-transport-16 §9.9 (SUBSCRIBE)
    // @see draft-ietf-moq-transport-16 §2.4.1 (namespace encoding)
    log('10. Subscribing to video track...');
    const videoReqId = await connection.subscribe(
        encodeNamespace(liveNamespace),
        enc.encode(videoTrack.name),
    );
    log(`   Video subscribed (requestId=${videoReqId}). Waiting for frames...`);
    videoTrackAlias = videoReqId;

    // ── 11. Subscribe to audio track ───────────────────────────────
    log('11. Subscribing to audio track...');
    if (audioTrack && audioDecoder) {
        const audioReqId = await connection.subscribe(
            encodeNamespace(liveNamespace),
            enc.encode(audioTrack.name),
        );
        log(`   Audio subscribed (requestId=${audioReqId}).`);
        audioTrackAlias = audioReqId;
    }

    startBtn.textContent = 'Playing';
}

// ─── Video object handler ────────────────────────────────────────────

/**
 * Handle a video MoqtObject.
 *
 * @see draft-ietf-moq-transport-16 §10.2.1.1 (Object Status)
 * @see draft-ietf-moq-loc-01 §2.2 (LOC payload = EncodedVideoChunk.data)
 * @see draft-ietf-moq-loc-01 §2.3.2.2 (VideoFrameMarking)
 * @see draft-ietf-moq-loc-01 §4.2 (ObjectID 0 = IDR frame)
 */
function handleVideoObject(obj: MoqtObject): void {
    // ── Gap handling ───────────────────────────────────────────────
    // @see draft-ietf-moq-transport-16 §10.2.1.1
    if (obj.kind === 'gap') {
        const status = Number(obj.status ?? 0n);
        if (status === 0x3) {
            log(`Video gap: End of Group (group=${obj.groupId}, objId=${obj.objectId})`);
            needsKeyframe = true;
        } else if (status === 0x4) {
            log(`Video gap: End of Track (group=${obj.groupId}, objId=${obj.objectId})`);
            log('Video track ended.');
        } else {
            log(`Video gap: status=0x${status.toString(16)} group=${obj.groupId}`);
            needsKeyframe = true;
        }
        return;
    }

    // ── Parse LOC headers ──────────────────────────────────────────
    // @see draft-ietf-moq-loc-01 §2.3 (LOC Header Extensions)
    const headers = parseLocHeaders(obj.properties);
    const chunkInit = toVideoChunkInit(obj.payload!, headers);

    // ── Keyframe gating ────────────────────────────────────────────
    const isKeyframe = chunkInit.type === 'key';

    if (needsKeyframe) {
        if (!isKeyframe) {
            return;
        }

        if (videoDecoder && videoConfigured) {
            videoDecoder.reset();
        }
        configureVideoDecoder();
        needsKeyframe = false;
        log(`Keyframe received (group=${obj.groupId}, objId=${obj.objectId})`);
    }

    // ── First keyframe ever — configure decoder ────────────────────
    if (!videoConfigured) {
        configureVideoDecoder();
        needsKeyframe = false;
    }

    // ── Decode ─────────────────────────────────────────────────────
    if (videoDecoder && videoDecoder.state === 'configured') {
        videoDecoder.decode(new EncodedVideoChunk(chunkInit));
    }
}

// ─── Audio object handler ────────────────────────────────────────────

/**
 * Handle an audio MoqtObject.
 *
 * Audio is simpler than video — every LOC audio object is independently
 * decodable (no keyframe gating needed).
 *
 * @see draft-ietf-moq-loc-01 §4.1 (Audio: each chunk independently decodable)
 * @see draft-ietf-moq-loc-01 §2.2 (LOC payload = EncodedAudioChunk.data)
 */
function handleAudioObject(obj: MoqtObject): void {
    if (obj.kind === 'gap') return;
    if (!audioDecoder || audioDecoder.state !== 'configured') return;

    const payload = obj.payload!;

    // Parse LOC headers for CaptureTimestamp; fall back to sequential.
    // Normalize to zero-based timestamps for decoder stability.
    // @see draft-ietf-moq-loc-01 §2.3.1.1 (CaptureTimestamp in microseconds)
    // 1024 samples per AAC-LC frame
    const frameDurationUs = Math.round(1024 / audioSampleRate * 1_000_000);
    let timestamp = audioSubmitCount * frameDurationUs;
    try {
        const headers = parseLocHeaders(obj.properties);
        if (headers.captureTimestamp !== undefined) {
            timestamp = Number(headers.captureTimestamp);
        }
    } catch {
        // LOC parse failed — use sequential timestamp
    }

    // Normalize to zero-based
    if (firstAudioTimestamp === null) firstAudioTimestamp = timestamp;
    timestamp -= firstAudioTimestamp;

    // Wrap raw AAC access unit in ADTS for Chrome's decoder.
    // LOC payload is the raw access unit (§2.2); ADTS framing is client-side only.
    const adtsFrame = wrapInADTS(payload, audioSampleRate, audioChannels);

    // Record frame info for error diagnostics (async errors fire later)
    lastAudioFrameInfo = `${payload.length}B`;

    // AAC-LC frames are always independently decodable → type 'key'.
    // @see W3C AAC WebCodecs Registration: type is always "key" for AAC
    // @see draft-ietf-moq-loc-01 §4.1 (each audio object independently decodable)
    audioDecoder.decode(new EncodedAudioChunk({
        type: 'key',
        timestamp,
        duration: frameDurationUs,
        data: adtsFrame,
    }));

    audioSubmitCount++;
}

// ─── Decoder configuration ───────────────────────────────────────────

/**
 * Configure the VideoDecoder with codec parameters from the catalog.
 *
 * @see draft-ietf-moq-msf-00 §5.1.24 (codec → VideoDecoderConfig.codec)
 * @see draft-ietf-moq-msf-00 §5.1.29 (width → codedWidth)
 * @see draft-ietf-moq-msf-00 §5.1.30 (height → codedHeight)
 * @see draft-ietf-moq-msf-00 §5.1.20 (initData → description)
 */
function configureVideoDecoder(): void {
    if (!videoDecoder) return;

    const config: VideoDecoderConfig = {
        codec: videoCodec,
        codedWidth: videoWidth,
        codedHeight: videoHeight,
    };

    if (videoInitData) {
        config.description = videoInitData;
    }

    videoDecoder.configure(config);
    videoConfigured = true;
}
