package org.projectcane.cane.extract;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/** Turns a decompressed level into the PCBRX v17 file the game loads (native port of upstream's Sly1BrxRebuilder). */
public final class BrxRebuild {
    private BrxRebuild() {}

    static { System.loadLibrary("brxrebuild"); }

    private static native String nativeRebuild(String path);

    /** "PCBR" little-endian + format version 17, the header transition.cpp insists on. */
    public static boolean isRebuilt(File f) {
        if (!f.isFile() || f.length() < 8) return false;
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            byte[] h = new byte[8];
            r.readFully(h);
            return Bytes.le32(h, 0) == 0x52424350L && Bytes.le32(h, 4) == 17L;
        } catch (IOException e) {
            return false;
        }
    }

    /** True when Worlds/ holds at least one level and every level file is already in the v17 format. */
    public static boolean allLevelsRebuilt(File worlds) {
        int found = 0;
        File[] episodes = worlds.listFiles(File::isDirectory);
        if (episodes == null) return false;
        for (File episode : episodes) {
            File[] levels = episode.listFiles(File::isDirectory);
            if (levels == null) continue;
            for (File level : levels) {
                File[] brx = level.listFiles((d, n) -> n.endsWith(".brx") && !n.startsWith("."));
                if (brx == null) continue;
                for (File f : brx) {
                    if (!isRebuilt(f)) return false;
                    found++;
                }
            }
        }
        return found > 0;
    }

    /** Rebuilds {@code decompressed} (must end in .brx) and moves the result to {@code dst}. */
    static void rebuildInto(File decompressed, File dst) throws IOException {
        String name = decompressed.getName();
        File rebuilt = new File(decompressed.getParentFile(), name.substring(0, name.length() - 4) + "_Rebuilt.brx");
        try {
            String err = nativeRebuild(decompressed.getPath());
            if (err != null) throw new IOException("Failed to rebuild " + dst.getName() + ": " + err);
            if (!rebuilt.isFile()) throw new IOException("The level rebuilder did not produce an output file for " + dst.getName());
            Fs.commit(rebuilt, dst);
        } finally {
            decompressed.delete();
            rebuilt.delete();
        }
    }
}
