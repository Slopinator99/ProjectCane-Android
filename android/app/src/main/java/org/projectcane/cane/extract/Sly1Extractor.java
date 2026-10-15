package org.projectcane.cane.extract;

import java.io.File;
import java.io.IOException;

/** Entry point: validates the ISO and extracts everything the game needs into {@code output}. */
public final class Sly1Extractor {
    private Sly1Extractor() {}

    public static final String IN_PROGRESS_MARKER = ".extracting";

    public static void run(IsoSource iso, File output, String dialogTable, ProgressListener pl) throws IOException {
        IsoDetector.Result r = IsoDetector.detect(iso);
        if (!r.valid) throw new IOException(r.message);
        Fs.mkdirs(output);
        File marker = new File(output, IN_PROGRESS_MARKER);
        Fs.writeAtomic(marker, new byte[] {1});
        LevelExtractor.extract(iso, output, r.executableOffset, pl, 0.0, 0.55);
        MediaExtractor.extract(iso, output, r.executableOffset, dialogTable, pl, 0.55, 0.45);
        marker.delete();
        pl.onProgress("Done", 1.0);
    }
}
