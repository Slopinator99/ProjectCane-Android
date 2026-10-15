package org.projectcane.cane.extract;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

final class Fs {
    private Fs() {}

    static void mkdirs(File d) throws IOException {
        if (!d.isDirectory() && !d.mkdirs() && !d.isDirectory()) throw new IOException("Cannot create " + d);
    }

    /** Writes via a .part file and renames, so an interrupted run never leaves a truncated file. */
    static void writeAtomic(File dst, byte[] data) throws IOException {
        mkdirs(dst.getParentFile());
        File tmp = new File(dst.getPath() + ".part");
        try (OutputStream os = new FileOutputStream(tmp)) { os.write(data); }
        commit(tmp, dst);
    }

    static void writeLines(File dst, List<String> lines) throws IOException {
        StringBuilder sb = new StringBuilder();
        for (String l : lines) sb.append(l).append('\n');
        writeAtomic(dst, sb.toString().getBytes(StandardCharsets.UTF_8));
    }

    static void commit(File tmp, File dst) throws IOException {
        if (dst.exists() && !dst.delete()) throw new IOException("Cannot replace " + dst);
        if (!tmp.renameTo(dst)) throw new IOException("Cannot rename " + tmp + " to " + dst);
    }

    static boolean done(File f) { return f.isFile() && f.length() > 0; }
}
