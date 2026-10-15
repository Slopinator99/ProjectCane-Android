package org.projectcane.cane.extract;

final class Bytes {
    private Bytes() {}
    static long le32(byte[] d, int o) {
        return (d[o] & 0xFFL) | ((d[o + 1] & 0xFFL) << 8) | ((d[o + 2] & 0xFFL) << 16) | ((d[o + 3] & 0xFFL) << 24);
    }
    static long be32(byte[] d, int o) {
        return (d[o + 3] & 0xFFL) | ((d[o + 2] & 0xFFL) << 8) | ((d[o + 1] & 0xFFL) << 16) | ((d[o] & 0xFFL) << 24);
    }
    static int be16(byte[] d, int o) { return ((d[o] & 0xFF) << 8) | (d[o + 1] & 0xFF); }
    static int u8(byte[] d, int o) { return d[o] & 0xFF; }
    static int toInt(long v) {
        if (v < 0 || v > Integer.MAX_VALUE) throw new ArithmeticException("Value too large: " + v);
        return (int) v;
    }
}
