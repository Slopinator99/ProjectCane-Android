package org.projectcane.cane.extract;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;

/** Random-access read-only view of the disc image. */
public final class IsoSource {
    private final FileChannel channel;
    private final long length;

    public IsoSource(FileChannel channel) throws IOException {
        this.channel = channel;
        this.length = channel.size();
    }

    public long length() { return length; }

    /** Reads exactly {@code size} bytes at {@code offset}; throws if outside the image. */
    public byte[] read(long offset, int size) throws IOException {
        if (offset < 0 || size < 0 || offset > length || size > length - offset)
            throw new IOException("The requested data is outside the ISO.");
        byte[] b = new byte[size];
        ByteBuffer bb = ByteBuffer.wrap(b);
        long pos = offset;
        while (bb.hasRemaining()) {
            int n = channel.read(bb, pos);
            if (n < 0) throw new IOException("Unexpected end of ISO.");
            pos += n;
        }
        return b;
    }

    /** Returns null instead of throwing when the range is outside the image. */
    public byte[] tryRead(long offset, int size) throws IOException {
        if (offset < 0 || size < 0 || offset > length || size > length - offset) return null;
        return read(offset, size);
    }
}
