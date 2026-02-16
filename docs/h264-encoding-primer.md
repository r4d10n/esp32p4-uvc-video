# H.264 Encoding Primer

A reference for the H.264 concepts used in this project's USB UVC and RTSP streaming configuration.

## Frame Types

### IDR Frame (Instantaneous Decoder Refresh)

A complete image encoded independently -- no reference to any other frame. A decoder can start playback from any IDR frame without needing prior frames. These are the largest frames in the stream. The ESP32-P4 hardware encoder produces Constrained Baseline profile, so all intra-frames are IDR.

### I-frame (Intra Frame)

Same idea as IDR -- self-contained, no dependencies. In Constrained Baseline, every I-frame is also an IDR. The distinction matters in higher profiles where I-frames can exist without resetting the reference chain, but not here.

### P-frame (Predicted Frame)

Encodes only the *differences* from the previous frame. Instead of storing 1920x1080 pixels, it stores motion vectors ("this 16x16 block moved 3 pixels right") plus a small residual correction. Much smaller than IDR frames -- typically 5-20x smaller for typical video.

### B-frame (Bidirectional Predicted)

B-frames reference both a **past** and a **future** frame to encode differences. They look both directions for the best motion prediction:

```
Display order:  IDR  B  B  P  B  B  P  B  B  IDR
Decode order:   IDR  P  B  B  P  B  B  IDR  B  B
                 ↑       ↑↓      ↑↓       ↑
              reference  refs   refs    reference
```

A B-frame between an IDR and a P-frame can say "this block is 60% like the IDR + 40% like the upcoming P-frame." This interpolation compresses better than P-frames alone -- typically 2-3x smaller than an equivalent P-frame.

**The catch:** frames must be encoded and sent **out of display order**. The decoder needs the future P-frame before it can decode the B-frames that reference it. This adds latency equal to the number of consecutive B-frames (typically 1-3 frame periods = 33-100ms at 30fps).

**Not available on the ESP32-P4.** The hardware H.264 encoder supports only **Constrained Baseline profile**, which explicitly forbids B-frames. The profile hierarchy:

| Profile | Frame Types | Use Case |
|---------|-------------|----------|
| **Constrained Baseline** | IDR, P only | Low-latency, embedded, video conferencing |
| Main | IDR, P, B | Broadcast TV, streaming |
| High | IDR, P, B + more tools | Blu-ray, high-quality archival |

Constrained Baseline was chosen for the ESP32-P4 encoder because it avoids the reorder buffer and keeps decode latency to a single frame -- appropriate for a webcam/IP camera.

## GOP (Group of Pictures)

The repeating pattern of frame types. Controlled by the `I_PERIOD` setting in Kconfig.

```
GOP=1:   IDR IDR IDR IDR IDR IDR ...        (all IDR, no compression benefit)
GOP=10:  IDR P P P P P P P P P IDR P P ...  (IDR every 10 frames)
GOP=30:  IDR P P P P ... P IDR ...           (IDR every 30 frames = 1 per second at 30fps)
```

**Trade-off:**
- **Small GOP** (1-5): Fast random access, resilient to packet loss, but high bitrate. This project uses GOP=1 for USB UVC because USB is lossless and hosts expect instant seek.
- **Large GOP** (10-30): Much better compression, but a lost IDR means corrupted video until the next IDR. This project uses GOP=10 for RTSP -- an IDR every 333ms at 30fps balances quality against network recovery.

This is why USB H.264 runs at ~120 Mbps with GOP=1 (all IDR) and drops to ~12 Mbps with GOP=10.

## QP (Quantization Parameter)

Controls the quality/size trade-off for each frame. Range 0-51:

| QP | Effect |
|----|--------|
| 0-15 | Near-lossless, very large frames |
| 16-25 | High quality, large frames |
| 26-35 | Good quality, reasonable size |
| 36-45 | Visible compression artifacts |
| 46-51 | Heavy blocking, very small frames |

The encoder picks a QP within the `[min_qp, max_qp]` range to hit the target bitrate:

- **min_qp** constrains the *best* quality (prevents wasting bits on already-good frames).
- **max_qp** constrains the *worst* quality (prevents ugly frames during high motion).

The encoder's rate control loop works like this each frame:

```
if (bitrate running too high) → increase QP (more compression, lower quality)
if (bitrate running too low)  → decrease QP (less compression, higher quality)
clamp QP to [min_qp, max_qp]
```

### Project Settings

| Transport | Bitrate | QP Range | Rationale |
|-----------|---------|----------|-----------|
| USB UVC | 1 Mbps | 25-50 | Low bandwidth, USB is lossless |
| RTSP Ethernet | 8 Mbps | 20-38 | High quality, 100Mbps headroom |

## Reference Frames

Reference frames are the frames that P-frames and B-frames point back to for motion prediction. The encoder maintains a **Decoded Picture Buffer (DPB)** of reference frames.

**Single reference (ESP32-P4):**
```
IDR ← P ← P ← P ← P
 Each P references only the immediately preceding frame
```

**Multiple references (Main/High profile encoders):**
```
IDR ← P ← P ← P ← P
 ↑              ↑
 └──── P ───────┘
       This P can reference both the IDR and a recent P
```

Multiple reference frames let the encoder pick the best match from several candidates. A scene with periodic repetition (pendulum, walking) benefits because a frame 10 frames ago might be a closer match than the previous frame.

| References | Compression | Memory | Decode Cost |
|------------|-------------|--------|-------------|
| 1 (ESP32-P4) | Baseline | 1 frame buffer (~4MB at 1080p) | Minimal |
| 2-4 | ~5-10% better | 2-4x buffer | Moderate |
| 16 (max) | Diminishing returns | 16x buffer | High |

Constrained Baseline limits this to **1 reference frame**, which is why the encoder chain is simple: each P-frame references only the frame immediately before it. An IDR resets the chain entirely -- anything before it is discarded from the DPB.

### Impact on Packet Loss (RTSP)

With 1 reference and no B-frames, a lost P-frame corrupts all subsequent P-frames until the next IDR (each one references the previous, propagating the error):

```
GOP=10, packet loss hits frame 3:

IDR  P  P  [P]  P̃  P̃  P̃  P̃  P̃  P̃  IDR  P  P  P  ...
          lost  ↑ corrupted until ↑    ↑ clean again
                    next IDR
```

With GOP=10 at 30fps, the worst-case corruption window is 333ms. With GOP=30, it would be 1 second.

## Entropy Coding: CAVLC vs CABAC

The two methods H.264 uses to compress the final bitstream -- the last step after prediction and quantization. They encode the same data (transform coefficients, motion vectors, mode decisions) into a compact binary stream.

### CAVLC (Context-Adaptive Variable-Length Coding)

Uses lookup tables of variable-length codes (like Huffman coding). "Context-adaptive" means it picks different code tables based on surrounding data -- if neighboring blocks had many non-zero coefficients, it expects this block will too and uses a table optimized for that.

```
Quantized coefficients: [3, 1, -1, 0, 0, 0, 0, ...]

Step 1: Count non-zero coefficients → 3
Step 2: Look up code tables based on neighbor context
Step 3: Encode each value with variable-length code from table
        3  → 0010  (short code, common value)
        1  → 1     (very short, most common)
       -1  → 01
```

**Fast** -- table lookups are simple integer operations. Trivial to implement in hardware.

### CABAC (Context-Adaptive Binary Arithmetic Coding)

Arithmetic coding encodes the entire symbol sequence as a single fractional number between 0 and 1. Each symbol narrows the interval based on its probability, and those probabilities are updated after every single symbol.

```
Quantized coefficients: [3, 1, -1, 0, 0, 0, 0, ...]

Step 1: Binarize each value into bit decisions
        3 → 1,1,1,0 (unary) + sign
Step 2: For each bit, use context model to estimate P(0) vs P(1)
Step 3: Narrow arithmetic interval by that probability
Step 4: Update the context model with what actually happened
Step 5: Repeat for every bit of every coefficient
```

**10-15% better compression** than CAVLC at the same quality. The probability models adapt per-bit, tracking statistics far more granularly than CAVLC's per-block table switching.

**Expensive** -- each bit requires a multiply, a comparison, and a model update. Inherently serial (each bit depends on the previous probability state).

### Profile Restrictions

| Profile | Entropy Coding | Why |
|---------|---------------|-----|
| **Constrained Baseline** | CAVLC only | Hardware simplicity, low latency |
| Main | CAVLC or CABAC | Broadcast balance |
| High | CAVLC or CABAC | Maximum flexibility |

**The ESP32-P4 uses CAVLC exclusively** because it implements Constrained Baseline.

### Practical Impact

At RTSP settings (8 Mbps target, 1080p):

```
CAVLC (ESP32-P4):  8 Mbps target → actual ~12 Mbps, good quality
CABAC (if available):  8 Mbps target → same quality at ~7 Mbps
                       or better quality at the same 8 Mbps
```

The ~12 Mbps the encoder produces at 1080p over 100 Mbps Ethernet is well within budget, so the CAVLC overhead is irrelevant for this use case. CABAC matters more when bandwidth is tight -- satellite TV, mobile streaming, or 4K where every bit counts.

### Why CABAC Is Hard in Hardware

CAVLC can process multiple coefficients per clock cycle since table lookups are independent. CABAC is bit-serial -- each bit's coding depends on the updated probability from the previous bit:

```
CAVLC: [coeff0] [coeff1] [coeff2] [coeff3]   ← parallel, ~1 cycle each

CABAC: bit0 → update → bit1 → update → bit2 → update → ...   ← serial
```

Hardware CABAC encoders exist (every phone SoC has one), but they're substantially larger and more power-hungry than CAVLC -- not justified for an embedded microcontroller like the ESP32-P4.

## H.264 Encoding Pipeline Summary

The full encoding pipeline for each frame:

```
Raw frame (1920x1080 UYVY, 4.1MB)
    │
    ▼
1. Partition into 16x16 macroblocks (120×68 = 8160 MBs)
    │
    ▼
2. Predict each MB:
   - IDR: Intra prediction (spatial, from neighboring MBs in same frame)
   - P:   Inter prediction (temporal, motion search in reference frame)
    │
    ▼
3. Subtract prediction → residual (what the prediction got wrong)
    │
    ▼
4. Transform (4x4 integer DCT) → frequency-domain coefficients
    │
    ▼
5. Quantize (divide by QP-derived step size) → small integers, many zeros
    │
    ▼
6. Entropy code (CAVLC) → compressed bitstream
    │
    ▼
7. NAL unit packaging → Annex B byte stream (0x00000001 start codes)
    │
    ▼
8a. USB UVC: wrap in UVC payload header → USB bulk transfer
8b. RTSP/RTP: split NALs into RTP packets (FU-A if >1400 bytes) → UDP
```

All steps 1-7 happen in the ESP32-P4 hardware H.264 encoder. Steps 8a/8b are handled in software by `uvc_streaming.c` and `rtp_sender.c` respectively.
