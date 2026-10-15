package org.projectcane.cane.extract;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** Port of Sly1ExtractionService: decompresses the 46 level .brx files. */
public final class LevelExtractor {
    static final int LEVEL_COUNT = 46, LEVEL_INFO_SIZE = 44, SECTOR = 0x800, CHUNK = 0x2000, MAX_MATCH = 10;
    static final int MAX_NAME = 256;
    static final long MAX_COMPRESSED = 256L * 1024 * 1024;
    static final long LEVEL_TABLE_VA = 0x00247AF0L, ELF_VA = 0x00100000L, ELF_FILE_OFFSET = 0x1000L;
    static final long LEVEL_TABLE_EXE_OFFSET = LEVEL_TABLE_VA - ELF_VA + ELF_FILE_OFFSET;

    private LevelExtractor() {}

    private static final class Level {
        final long id; final String name; final long offset; final int size;
        Level(long id, String name, long offset, int size) { this.id = id; this.name = name; this.offset = offset; this.size = size; }
    }

    /** Reports 0..1 of the level phase through {@code onFraction}. */
    public static void extract(IsoSource iso, File output, long exeOffset, ProgressListener pl, double base, double span) throws IOException {
        File worlds = new File(output, "Worlds");
        Fs.mkdirs(worlds);
        byte[] table = iso.read(exeOffset + LEVEL_TABLE_EXE_OFFSET, LEVEL_COUNT * LEVEL_INFO_SIZE);

        List<Level> levels = new ArrayList<>();
        long totalCompressed = 0;
        for (int i = 0; i < LEVEL_COUNT; i++) {
            int o = i * LEVEL_INFO_SIZE;
            long sector = Bytes.le32(table, o) ^ Bytes.le32(table, o + 20);
            long size = Bytes.le32(table, o + 4) ^ Bytes.le32(table, o + 28);
            long levelId = Bytes.le32(table, o + 32);
            long nameVa = Bytes.le32(table, o + 36);
            if (size == 0 || size > MAX_COMPRESSED)
                throw new IOException("Level " + i + " has an invalid compressed size: " + size + ".");
            long dataOffset = sector * SECTOR;
            if (dataOffset > iso.length() || size > iso.length() - dataOffset)
                throw new IOException("Level " + i + " points outside the ISO.");
            String name = readExeString(iso, exeOffset, nameVa);
            name = sanitize(name == null || name.trim().isEmpty() ? "Level_" + i : name);
            if ((levelId & 0xFFFFL) == 3L) name = "Paris";
            levels.add(new Level(levelId, name, dataOffset, (int) size));
            totalCompressed += size;
        }

        long processed = 0;
        for (int i = 0; i < levels.size(); i++) {
            if (pl.isCancelled()) throw new IOException("Cancelled.");
            Level lv = levels.get(i);
            String label = "Worlds / " + episode(lv.id) + " / " + lv.name;
            pl.onProgress(label, base + span * processed / (double) totalCompressed);
            File dir = new File(new File(worlds, episode(lv.id)), lv.name);
            Fs.mkdirs(dir);
            File dst = new File(dir, lv.name + ".brx");
            // Files from before the rebuilder existed (plain decompressed levels) are redone, the game rejects them.
            if (!BrxRebuild.isRebuilt(dst)) {
                byte[] compressed = iso.read(lv.offset, lv.size);
                File tmp = new File(dir, "." + lv.name + ".tmp.brx");
                try (OutputStream os = new BufferedOutputStream(new FileOutputStream(tmp), 1 << 20)) {
                    decompress(compressed, os);
                }
                pl.onProgress("Rebuilding " + label, base + span * processed / (double) totalCompressed);
                BrxRebuild.rebuildInto(tmp, dst);
            }
            processed += lv.size;
        }
        pl.onProgress("Worlds done", base + span);
    }

    /** LZ decoder, streamed to {@code out} in 0x2000-byte chunks exactly as the original. */
    static void decompress(byte[] input, OutputStream out) throws IOException {
        byte[] work = new byte[CHUNK + MAX_MATCH];
        int in = 0, outPos = 0;
        int flags = 0;
        while (in < input.length) {
            flags >>>= 1;
            if ((flags & 0x100) == 0) {
                if (in >= input.length) break;
                flags = 0xFF00 | (input[in++] & 0xFF);
            }
            if (in >= input.length) break;
            int first = input[in++] & 0xFF;
            if ((flags & 1) != 0) {
                work[outPos++] = (byte) first;
            } else {
                if (in >= input.length) break;
                int second = input[in++] & 0xFF;
                int token = first | (second << 8);
                int src = token & 0x1FFF;
                int len = (token >> 13) + 3;
                for (int k = 0; k < len; k++) work[outPos++] = work[src++ & 0x1FFF];
            }
            if (outPos < CHUNK) continue;
            out.write(work, 0, CHUNK);
            int spill = outPos - CHUNK;
            if (spill > 0) System.arraycopy(work, CHUNK, work, 0, spill);
            outPos = spill;
        }
        out.write(work, 0, outPos);
    }

    static String readExeString(IsoSource iso, long exeOffset, long va) throws IOException {
        if (va < ELF_VA) return null;
        long off = exeOffset + (va - ELF_VA) + ELF_FILE_OFFSET;
        if (off < 0 || off >= iso.length()) return null;
        int n = (int) Math.min(MAX_NAME, iso.length() - off);
        byte[] b = iso.read(off, n);
        for (int i = 0; i < b.length; i++)
            if (b[i] == 0) return new String(b, 0, i, StandardCharsets.US_ASCII);
        return null;
    }

    static String sanitize(String name) {
        char[] c = name.toCharArray();
        for (int i = 0; i < c.length; i++) {
            char ch = c[i];
            if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|'
                    || ch == '?' || ch == '*' || Character.isISOControl(ch)) c[i] = '_';
        }
        String s = new String(c);
        int end = s.length();
        while (end > 0 && (s.charAt(end - 1) == ' ' || s.charAt(end - 1) == '.')) end--;
        s = s.substring(0, end);
        return s.trim().isEmpty() ? "Unnamed Level" : s;
    }

    static String episode(long id) {
        switch ((int) (id >> 8)) {
            case 1: return "Tide of Terror";
            case 2: return "Sunset Snake Eyes";
            case 3: return "Vicious Voodoo";
            case 4: return "Fire in the sky";
            case 5: return "The Cold Heart of Hate";
            default: return "";
        }
    }
}
