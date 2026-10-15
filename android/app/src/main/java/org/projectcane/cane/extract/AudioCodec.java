package org.projectcane.cane.extract;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Port of Sly1AudioCodec: Sony VAG/ADPCM decoding and PSS (MPEG-PS) demuxing. */
public final class AudioCodec {
    private AudioCodec() {}

    /** Sliding read window over a region of the ISO, so a 350 MB cutscene never has to fit in the Java heap. */
    private static final class Window {
        private final IsoSource iso;
        private final long end;
        private final byte[] buf = new byte[1 << 20];
        private long start;
        private int len;

        Window(IsoSource iso, long offset, long size) { this.iso = iso; this.end = offset + size; this.start = offset; }

        /** Makes [off, off+n) addressable and returns its index in {@link #buf}, or -1 if it runs past the region. */
        int at(long off, int n) throws IOException {
            if (off < 0 || n < 0 || off + n > end) return -1;
            if (off < start || off + n > start + len) {
                int want = (int) Math.min(buf.length, end - off);
                byte[] chunk = iso.read(off, want);
                System.arraycopy(chunk, 0, buf, 0, want);
                start = off;
                len = want;
            }
            return (int) (off - start);
        }
    }

    public static byte[] decodeVag(byte[] vag) throws IOException {
        if (vag.length < 0x30 || vag[0] != 'V' || vag[1] != 'A' || vag[2] != 'G' || vag[3] != 'p')
            throw new IOException("Missing Sony VAGp header.");
        long declared = Bytes.be32(vag, 0x0C);
        long rate = Bytes.be32(vag, 0x10);
        if (rate < 4000 || rate > 192000) throw new IOException("Invalid VAG sample rate " + rate + ".");
        int available = vag.length - 0x30;
        int size = declared == 0 ? available : (int) Math.min(declared, (long) available);
        size &= ~15;
        return writeWav(decodeAdpcm(vag, 0x30, size), null, 1, rate);
    }

    /**
     * Demuxes the PSS at {@code isoOffset}..+{@code size}: video packets are written to {@code videoOut} as they are
     * read; audio is decoded and streamed to {@code audioOut} as a WAV. Returns the video byte count.
     */
    public static long demuxPss(IsoSource iso, long isoOffset, long size, OutputStream videoOut, OutputStream audioOut)
            throws IOException {
        final int END = 0x000001B9, PACK = 0x000001BA, SYSTEM = 0x000001BB;
        final int PRIVATE = 0x000001BD, PADDING = 0x000001BE, VIDEO = 0x000001E0;
        Window w = new Window(iso, isoOffset, size);
        final long limit = isoOffset + size;
        long videoBytes = 0;
        ByteArrayOutputStream sonyAds = new ByteArrayOutputStream();
        ByteArrayOutputStream directAudio = new ByteArrayOutputStream();
        long format = 0, rate = 0, interleave = 0x800;
        int channels = 0;
        boolean firstAudio = true;
        long pos = isoOffset;

        while (pos + 4 <= limit) {
            int i = w.at(pos, 4);
            int id = (int) Bytes.be32(w.buf, i);
            if (id == END) break;
            if (id == PACK) {
                if ((i = w.at(pos, 14)) < 0) throw new IOException("Truncated PSS pack header.");
                pos += 14 + (w.buf[i + 13] & 7);
                continue;
            }
            if (id == SYSTEM || id == PADDING) {
                if ((i = w.at(pos, 6)) < 0) throw new IOException("Truncated PSS packet.");
                pos += 6 + Bytes.be16(w.buf, i + 4);
                continue;
            }
            if (id != VIDEO && id != PRIVATE) { pos++; continue; }
            if ((i = w.at(pos, 9)) < 0) throw new IOException("Truncated PSS PES packet.");
            int packetSize = Bytes.be16(w.buf, i + 4);
            if (pos + 6 + packetSize > limit) throw new IOException("Invalid PSS packet size.");
            i = w.at(pos, 6 + packetSize); // whole packet (<= 64 KB) is now addressable
            int extension = Bytes.u8(w.buf, i + 8);
            int payload = i + 9 + extension;
            int payloadSize = packetSize - 3 - extension;
            if (payloadSize < 0 || pos + 9 + extension + payloadSize > limit) throw new IOException("Invalid PSS payload.");

            if (id == VIDEO) {
                videoOut.write(w.buf, payload, payloadSize);
                videoBytes += payloadSize;
            } else if (payloadSize > 4) {
                // Each packet feeds exactly one buffer: sonyAds is only consulted when no marker-1 packet exists.
                if (w.buf[payload + 3] != 1) sonyAds.write(w.buf, payload + 4, payloadSize - 4);
                else {
                    int audioOffset = payload + 4;
                    int audioSize = payloadSize - 4;
                    if (firstAudio) {
                        if (audioSize < 0x800) throw new IOException("Truncated first PSS audio packet.");
                        rate = Bytes.be16(w.buf, audioOffset + 6);
                        channels = Bytes.u8(w.buf, audioOffset + 8);
                        format = 2;
                        audioOffset += 0x800;
                        audioSize -= 0x800;
                        firstAudio = false;
                    }
                    directAudio.write(w.buf, audioOffset, audioSize);
                }
            }
            pos += 6 + packetSize;
        }

        // The audio is ~25 MB for the longest cutscene but is copied several times below; drop each copy as soon as
        // it is consumed so the peak stays well inside the 512 MB Android heap.
        byte[] adpcm = directAudio.toByteArray();
        directAudio = null;
        if (adpcm.length != 0) sonyAds = null;
        if (adpcm.length == 0) {
            byte[] ads = sonyAds.toByteArray();
            sonyAds = null;
            int header = find(ads, new byte[] {'S', 'S', 'h', 'd'}, 0);
            int body = header < 0 ? -1 : find(ads, new byte[] {'S', 'S', 'b', 'd'}, header + 8);
            if (header < 0 || body < 0 || header + 32 > ads.length || body + 8 > ads.length)
                throw new IOException("PSS audio headers were not found.");
            format = Bytes.le32(ads, header + 8);
            rate = Bytes.le32(ads, header + 12);
            channels = (int) Bytes.le32(ads, header + 16);
            if (channels < 0 || channels > 255) throw new IOException("Invalid PSS channel count.");
            interleave = Bytes.le32(ads, header + 20);
            int bodySize = (int) Math.min(Bytes.le32(ads, body + 4), (long) (ads.length - body - 8));
            adpcm = new byte[bodySize];
            System.arraycopy(ads, body + 8, adpcm, 0, bodySize);
        }

        if (videoBytes == 0 || adpcm.length == 0 || channels < 1 || channels > 2 || rate == 0)
            throw new IOException("The PSS streams are incomplete.");

        int block = (int) (interleave == 0 ? 0x800 : interleave);
        ByteArrayOutputStream c0 = new ByteArrayOutputStream(adpcm.length / channels + block);
        ByteArrayOutputStream c1 = new ByteArrayOutputStream(channels == 2 ? adpcm.length / 2 + block : 0);
        if (channels == 1) {
            c0.write(adpcm, 0, adpcm.length);
        } else {
            int position = 0, channel = 0;
            while (position < adpcm.length) {
                int count = Math.min(block, adpcm.length - position);
                (channel == 0 ? c0 : c1).write(adpcm, position, count);
                position += count;
                channel ^= 1;
            }
        }
        adpcm = null;

        short[] left, right = new short[0];
        byte[] b0 = c0.toByteArray();
        c0 = null;
        byte[] b1 = c1.toByteArray();
        c1 = null;
        if (format == 2 || format == 0x10) {
            left = decodeAdpcm(b0, 0, b0.length);
            b0 = null;
            if (channels == 2) right = decodeAdpcm(b1, 0, b1.length);
        } else if (format == 0 || format == 1) {
            left = decodePcm(b0, format == 1);
            b0 = null;
            if (channels == 2) right = decodePcm(b1, format == 1);
        } else {
            throw new IOException("Unsupported PSS audio format " + format + ".");
        }
        b1 = null;
        writeWavTo(audioOut, left, right, channels, rate);
        return videoBytes;
    }

    private static final int[][] FILTERS = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

    static short[] decodeAdpcm(byte[] data, int start, int length) {
        short[] pcm = new short[Math.max(0, length / 16) * 28];
        int n = 0, h1 = 0, h2 = 0;
        for (int offset = 0; offset + 16 <= length; offset += 16) {
            int f = start + offset;
            int b0 = data[f] & 0xFF;
            int predictor = b0 >> 4, shift = b0 & 15;
            if (predictor >= 5) continue;
            boolean end = b0 == 0 && (data[f + 1] & 0xFF) == 1;
            for (int i = 2; end && i < 16; i++) end = (data[f + i] & 0xFF) == 0x77;
            if (end) break;
            for (int i = 0; i < 28; i++) {
                int packed = data[f + 2 + i / 2] & 0xFF;
                int nibble = (i & 1) != 0 ? packed >> 4 : packed & 15;
                if (nibble >= 8) nibble -= 16;
                int sample = (nibble << 12) >> shift;
                sample += (h1 * FILTERS[predictor][0] + h2 * FILTERS[predictor][1] + 32) >> 6;
                sample = Math.max(Short.MIN_VALUE, Math.min(Short.MAX_VALUE, sample));
                h2 = h1;
                h1 = sample;
                pcm[n++] = (short) sample;
            }
        }
        if (n == pcm.length) return pcm;
        short[] out = new short[n];
        System.arraycopy(pcm, 0, out, 0, n);
        return out;
    }

    private static short[] decodePcm(byte[] data, boolean littleEndian) {
        short[] s = new short[data.length / 2];
        for (int i = 0; i < s.length; i++) {
            int a = data[i * 2] & 0xFF, b = data[i * 2 + 1] & 0xFF;
            s[i] = (short) (littleEndian ? (a | (b << 8)) : ((a << 8) | b));
        }
        return s;
    }

    static byte[] writeWav(short[] left, short[] right, int channels, long rate) throws IOException {
        ByteArrayOutputStream os = new ByteArrayOutputStream();
        writeWavTo(os, left, right, channels, rate);
        return os.toByteArray();
    }

    /** Streams a 16-bit PCM WAV through a small buffer, so long cutscene audio is never held twice in memory. */
    static void writeWavTo(OutputStream os, short[] left, short[] right, int channels, long rate) throws IOException {
        int frames = channels == 2 ? Math.min(left.length, right == null ? 0 : right.length) : left.length;
        if (frames == 0) throw new IOException("Audio decoder produced no samples.");
        int dataSize = Math.multiplyExact(Math.multiplyExact(frames, channels), 2);
        ByteBuffer hdr = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN);
        hdr.put(new byte[] {'R', 'I', 'F', 'F'}).putInt(36 + dataSize);
        hdr.put(new byte[] {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}).putInt(16);
        hdr.putShort((short) 1).putShort((short) channels);
        hdr.putInt((int) rate).putInt((int) (rate * channels * 2));
        hdr.putShort((short) (channels * 2)).putShort((short) 16);
        hdr.put(new byte[] {'d', 'a', 't', 'a'}).putInt(dataSize);
        os.write(hdr.array());
        ByteBuffer bb = ByteBuffer.allocate(1 << 16).order(ByteOrder.LITTLE_ENDIAN);
        for (int i = 0; i < frames; i++) {
            if (bb.remaining() < 4) { os.write(bb.array(), 0, bb.position()); bb.clear(); }
            bb.putShort(left[i]);
            if (channels == 2) bb.putShort(right[i]);
        }
        os.write(bb.array(), 0, bb.position());
    }

    private static int find(byte[] data, byte[] pattern, int startIndex) {
        outer:
        for (int i = Math.max(0, startIndex); i <= data.length - pattern.length; i++) {
            for (int j = 0; j < pattern.length; j++) if (data[i + j] != pattern[j]) continue outer;
            return i;
        }
        return -1;
    }
}
