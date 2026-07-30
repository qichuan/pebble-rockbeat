#!/usr/bin/env python3
"""Generate watch/src/c/chart.c and watch/resources/data/music.pcm from an mp3.

    ./venv/bin/python tools/make_chart.py path/to/track.mp3

Everything the game plays comes out of this script, so the chart and the audio
can never drift apart -- both are cut from the same excerpt offsets below.

Why the audio is a raw PCM resource and not the mp3: PebbleOS ships no decoder,
and the Speaker API accepts raw PCM only (8/16kHz, 8/16-bit, mono). 8kHz 8-bit
is 8000 bytes/sec, so the 88s excerpt is 688KB against emery's 1024KB resource
budget. 16kHz would double that and not fit. The cost is a 4kHz ceiling.

Requires numpy and ffmpeg. numpy is not needed to BUILD the game, only to
re-generate the chart:

    python3 -m venv venv && ./venv/bin/pip install numpy
"""

import os
import subprocess
import sys
import wave

import numpy as np

# --- Excerpt -----------------------------------------------------------------
# Bar-aligned window into the source track. 33 bars covering the main peak, the
# breakdown and the second peak, stopping before the fade-out at ~180s.
BPM = 90.0
BEAT_MS = 60000.0 / BPM          # 666.667
BAR_MS = BEAT_MS * 4
PHASE_MS = 191.6                 # first downbeat in the source
START_BAR, END_BAR = 26, 48

# --- Analysis ----------------------------------------------------------------
SR = 22050
HOP, WIN = 256, 1024             # 11.6ms resolution
BANDS = {"low": (0, 200), "mid": (200, 1200), "high": (1200, 11025)}
LANE_OF_BAND = ["RB_LANE_BOT", "RB_LANE_MID", "RB_LANE_TOP"]  # low/mid/high

# --- Chart -------------------------------------------------------------------
LEAD_MS = 2000                   # music and first note share this origin
TAIL_MS = 2500

# Quantise to EIGHTHS (333ms at 90 BPM), not sixteenths.
#
# Sixteenths were the original choice and they were a mistake twice over. A
# sixteenth is 167ms, which at the scroll speed is ~17px of separation -- notes
# visually collide -- and, more importantly, a grid that fine lets the generator
# place notes on positions a listener does not hear as the beat, so the chart
# reads as noise rather than rhythm. On an eighth grid every note lands on a
# subdivision you can actually feel.
GRID_DIVISOR = 2                 # 2 = eighths, 4 = sixteenths

SNAP_TOLERANCE_MS = 110          # widened with the coarser grid
DROP_QUANTILE = 44               # drop the quietest N% of grid slots, per band
BIG_QUANTILE = 94
# One eighth at 90 BPM. Must stay above 2 * RB_MISS_MS (320ms) or two notes in a
# lane get overlapping judgment windows and stop being individually hittable.
SAME_LANE_MIN_MS = 333
# Floor on the gap between ANY two notes regardless of lane. Caps the density at
# ~3.3/s so a loud bar cannot turn into a wall of notes.
GLOBAL_MIN_MS = 300

# --- Audio encoding ----------------------------------------------------------
# Raw signed 8-bit PCM at 16kHz.
#
# 16kHz rather than 8kHz because 8kHz caps the audio at 4kHz and made the track
# sound like AM radio. Raw 8-bit rather than compressed because IMA ADPCM was
# tried and measured worse: at 4 bits/sample it returned 24.4dB SNR against
# 37.5dB for plain 8-bit on this material. ADPCM buys bandwidth by spending
# precision, and the speaker output stage is 8-bit anyway, so the trade lost.
#
# 16kHz 8-bit is 16000 bytes/sec, which is what forces the excerpt down to ~59s
# to stay inside emery's 1024KB resource budget. Length was traded for fidelity
# deliberately.
MUSIC_RATE = 16000

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WATCH = os.path.join(ROOT, "watch")

CHART_TEMPLATE = '''#include "chart.h"

// ---------------------------------------------------------------------------
// GENERATED FILE -- do not hand-edit. Regenerate with tools/make_chart.py,
// which also re-cuts watch/resources/data/music.pcm from the same excerpt so the
// chart and the audio cannot drift apart.
//
// "Prelude Drive" -- charted from the audio itself, not hand-placed.
//
// Source: a remix of Chopin's Prelude Op. 28 No. 4. The stage is a {dur_s:.1f}s
// excerpt from {start_s:.3f}s to {end_s:.3f}s of the original -- {bars} bars at
// {bpm} BPM, bar-aligned, taken from the track's sustained high-energy section.
//
// The length is set by audio quality, not by musical taste: the excerpt is
// stored as raw 16kHz 8-bit PCM at 16000 bytes/sec, so ~59s is what fits inside
// emery's 1024KB resource budget. See tools/make_chart.py.
//
// Pipeline: STFT (11.6ms hop) -> positive spectral flux per frequency band,
// averaged PER BIN and z-scaled -> peak-pick against a moving-average threshold
// -> tempo by comb-filtering the onset envelope -> snap to {grid_name}.
//
// The per-bin normalisation is load-bearing: the treble band has 456 FFT bins
// against the bass band's 10, so comparing raw summed flux reports "treble" for
// almost every onset.
//
// Lanes follow the dominant band, which is why the chart reads the way the music
// sounds: bass and kick on BOTTOM, piano body on MIDDLE, melody on TOP. The drop
// threshold is applied per band rather than globally, otherwise the loudest band
// crowds the others out and one lane goes nearly unused.
//
// {count} notes, {per_sec:.2f}/s. Spacing guarantees: no two notes in the SAME
// lane within {same_lane}ms (above 2*RB_MISS_MS, so judgment windows can never
// overlap -- tools/run_tests.sh asserts this), and no two notes at all within
// {global_min}ms, which caps how dense a loud bar can get.
// ---------------------------------------------------------------------------

static const ChartNote s_demo_notes[] = {{
{rows}
}};

// The music resource begins at RB_MUSIC_START_MS, the same offset the first note
// sits at, so the audio and the chart share one origin.
static const Chart s_demo_chart = {{
  .title = "Prelude Drive",
  .notes = s_demo_notes,
  .note_count = (uint16_t)(sizeof(s_demo_notes) / sizeof(s_demo_notes[0])),
  .bpm = {bpm},
  .lead_in_ms = {lead},
  .end_ms = {end_ms},
}};

const Chart *chart_get_builtin(void) {{
  return &s_demo_chart;
}}

bool chart_load_from_resource(uint32_t resource_id, Chart *out_chart) {{
  // Not implemented in v1 -- see the format documented in chart.h. Callers are
  // expected to fall back to chart_get_builtin().
  (void)resource_id;
  (void)out_chart;
  return false;
}}
'''


def decode(mp3, path, rate, fmt, extra=None, ss=None, t=None):
    cmd = ["ffmpeg", "-y", "-v", "error"]
    if ss is not None:
        cmd += ["-ss", f"{ss:.4f}"]
    if t is not None:
        cmd += ["-t", f"{t:.4f}"]
    cmd += ["-i", mp3]
    if extra:
        cmd += ["-af", extra]
    cmd += ["-ac", "1", "-ar", str(rate), "-f", fmt, path]
    subprocess.run(cmd, check=True)


def band_envelopes(x):
    nfr = (len(x) - WIN) // HOP
    win = np.hanning(WIN).astype(np.float32)
    frames = np.lib.stride_tricks.as_strided(
        x, shape=(nfr, WIN), strides=(x.strides[0] * HOP, x.strides[0])) * win
    spec = np.abs(np.fft.rfft(frames, axis=1)).astype(np.float32)
    freqs = np.fft.rfftfreq(WIN, 1.0 / SR)
    log = np.log1p(spec * 20.0)
    flux = np.maximum(0.0, np.diff(log, axis=0, prepend=log[:1]))

    out = {}
    for name, (lo, hi) in BANDS.items():
        mask = (freqs >= lo) & (freqs < hi)
        # MEAN per bin, then z-scale. Summing instead would make the comparison
        # meaningless: the high band has 456 bins against the low band's 10, so
        # a summed argmax reports "high" for essentially every onset.
        e = flux[:, mask].mean(axis=1)
        out[name] = e / (e.std() + 1e-9)
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    mp3 = sys.argv[1]

    analysis = os.path.join(ROOT, "tools", "_analysis.wav")
    decode(mp3, analysis, SR, "wav")
    with wave.open(analysis, "rb") as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
    os.remove(analysis)

    bands = band_envelopes(x)
    low, mid, high = bands["low"], bands["mid"], bands["high"]
    total = low + mid + high
    fps = SR / HOP

    # Peak-pick against a moving average so quiet passages still yield onsets
    # and loud ones do not spray them.
    w_frames = int(fps * 0.35)
    movavg = np.convolve(total, np.ones(w_frames) / w_frames, mode="same")
    thresh = movavg * 1.25 + 0.15 * total.std()
    cand = np.array([i for i in range(1, len(total) - 1)
                     if total[i] > thresh[i] and total[i] >= total[i - 1] and total[i] > total[i + 1]])
    t_ms = cand / fps * 1000.0
    strength = total[cand]
    dom = np.stack([low[cand], mid[cand], high[cand]]).argmax(axis=0)
    print(f"onsets: {len(cand)}  band split low/mid/high = "
          f"{(dom==0).sum()}/{(dom==1).sum()}/{(dom==2).sum()}")

    start = PHASE_MS + START_BAR * BAR_MS
    end = PHASE_MS + END_BAR * BAR_MS
    dur = end - start
    sel = (t_ms >= start) & (t_ms < end)
    t_sel, s_sel, d_sel = t_ms[sel], strength[sel], dom[sel]

    grid = BEAT_MS / GRID_DIVISOR
    slot = np.round((t_sel - start) / grid).astype(int)
    err = np.abs((t_sel - start) - slot * grid)
    keep = err < SNAP_TOLERANCE_MS
    print(f"excerpt {start/1000:.3f}-{end/1000:.3f}s ({dur/1000:.1f}s, {END_BAR-START_BAR} bars); "
          f"on-grid {keep.sum()}/{len(t_sel)}, median snap {np.median(err[keep]):.1f}ms")

    best = {}
    for k, s, d in zip(slot[keep], s_sel[keep], d_sel[keep]):
        if k not in best or s > best[k][0]:
            best[k] = (s, int(d))

    # Threshold PER BAND, not globally. A single global cut keeps whichever band
    # happens to be loudest and starves the others -- it produced a 19/46/56 lane
    # split, leaving the bass lane almost unused. Taking each band's own loudest
    # onsets keeps all three lanes in play.
    st_all = np.array([v[0] for v in best.values()])
    drop_by_band = {}
    for band in range(3):
        st_band = np.array([v[0] for v in best.values() if v[1] == band])
        drop_by_band[band] = (np.percentile(st_band, DROP_QUANTILE)
                              if len(st_band) else float("inf"))
    big_above = np.percentile(st_all, BIG_QUANTILE)

    last = {0: -1e9, 1: -1e9, 2: -1e9}
    last_any = -1e9
    notes = []
    for k in sorted(best):
        s, d = best[k]
        if s < drop_by_band[d]:
            continue
        t = LEAD_MS + k * grid
        if t - last_any < GLOBAL_MIN_MS:
            continue
        # Prefer the dominant band's lane; if it is still inside its judgment
        # window, move to a neighbour rather than dropping the note. That is what
        # makes fast passages alternate lanes instead of thinning out.
        for lane in (d, (d + 1) % 3, (d + 2) % 3):
            if t - last[lane] >= SAME_LANE_MIN_MS:
                notes.append((int(round(t)), lane, 1 if s >= big_above else 0))
                last[lane] = t
                last_any = t
                break
    notes.sort()

    for i in range(1, len(notes)):
        assert notes[i][0] >= notes[i - 1][0], "chart must be sorted"
    for lane in range(3):
        gaps = np.diff([t for t, l, _ in notes if l == lane])
        assert len(gaps) == 0 or gaps.min() >= SAME_LANE_MIN_MS, f"lane {lane} spacing violated"

    print(f"notes: {len(notes)} ({len(notes)/(dur/1000):.2f}/s), "
          f"big {sum(n[2] for n in notes)}, lanes " +
          "/".join(str(sum(1 for n in notes if n[1] == i)) for i in range(3)))

    # The audio excerpt. Compressed and limited before the bit-depth reduction --
    # 8-bit has ~48dB of range, so without it the quiet breakdown would vanish
    # into quantisation noise.
    pcm = os.path.join(WATCH, "resources", "data", "music.pcm")
    os.makedirs(os.path.dirname(pcm), exist_ok=True)
    decode(mp3, pcm, MUSIC_RATE, "s8",
           extra="highpass=f=80,dynaudnorm=f=250:g=5:p=0.9,alimiter=level_in=1:level_out=0.92",
           ss=start / 1000.0, t=dur / 1000.0)
    size = os.path.getsize(pcm)
    print(f"music.pcm: {size/1024:.1f}KB, {size/MUSIC_RATE:.1f}s at {MUSIC_RATE}Hz")

    rows = "\n".join(
        f"  {{ {t:6d}, {LANE_OF_BAND[l]:12s}, "
        f"{'RB_NOTE_BIG   ' if b else 'RB_NOTE_NORMAL'} }},"
        for t, l, b in notes)
    end_ms = int(LEAD_MS + dur + TAIL_MS)
    chart_c = os.path.join(WATCH, "src", "c", "chart.c")
    with open(chart_c, "w") as f:
        f.write(CHART_TEMPLATE.format(
            start_s=start / 1000.0, end_s=end / 1000.0, dur_s=dur / 1000.0,
            bars=END_BAR - START_BAR, bpm=int(BPM), grid_name=
            {2: "eighths", 4: "sixteenths"}.get(GRID_DIVISOR, f"1/{GRID_DIVISOR}"),
            count=len(notes), per_sec=len(notes) / (dur / 1000.0),
            same_lane=SAME_LANE_MIN_MS, global_min=GLOBAL_MIN_MS,
            rows=rows, lead=LEAD_MS, end_ms=end_ms))
    print(f"wrote {chart_c} ({len(notes)} notes, end_ms={end_ms})")


if __name__ == "__main__":
    main()
