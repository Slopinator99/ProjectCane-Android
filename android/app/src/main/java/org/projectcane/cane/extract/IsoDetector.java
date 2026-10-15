package org.projectcane.cane.extract;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Port of Sly1IsoDetector: confirms the image is Sly 1 NTSC-U (SCUS-97198) and locates the ELF. */
public final class IsoDetector {
    public static final class Result {
        public final boolean valid;
        public final String message;
        public final long executableOffset;
        public final long executableLength;
        Result(boolean valid, String message, long off, long len) {
            this.valid = valid; this.message = message; this.executableOffset = off; this.executableLength = len;
        }
    }

    private static final int SECTOR = 2048;
    private static final String EXPECTED_EXE = "SCUS_971.98";
    private static final Pattern BOOT = Pattern.compile(
            "^\\s*BOOT2\\s*=\\s*cdrom0:\\\\+([^;\\r\\n]+)(?:;\\d+)?\\s*$",
            Pattern.CASE_INSENSITIVE | Pattern.MULTILINE);

    private static final class Rec {
        final long extent; final int length; final boolean dir; final String name;
        Rec(long e, int l, boolean d, String n) { extent = e; length = l; dir = d; name = n; }
    }

    private IsoDetector() {}

    private static Result invalid(String m) { return new Result(false, m, 0, 0); }

    public static Result detect(IsoSource iso) {
        try {
            byte[] pvd = iso.tryRead(16L * SECTOR, SECTOR);
            if (pvd == null) return invalid("The ISO is too small or could not be read.");
            if (pvd[0] != 1 || !"CD001".equals(new String(pvd, 1, 5, StandardCharsets.US_ASCII)) || pvd[6] != 1)
                return invalid("The image does not contain a valid ISO9660 filesystem.");

            Rec root = readRecord(pvd, 156);
            if (root == null || !root.dir) return invalid("The ISO9660 root directory is invalid.");
            byte[] rootBytes = iso.tryRead(root.extent * SECTOR, root.length);
            if (rootBytes == null) return invalid("The ISO9660 root directory could not be read.");

            List<Rec> entries = readDirectory(rootBytes);
            Rec cnf = find(entries, "SYSTEM.CNF");
            if (cnf == null) return invalid("SYSTEM.CNF was not found in the ISO.");
            byte[] cnfBytes = iso.tryRead(cnf.extent * SECTOR, cnf.length);
            if (cnfBytes == null) return invalid("SYSTEM.CNF could not be read.");

            Matcher m = BOOT.matcher(new String(cnfBytes, StandardCharsets.US_ASCII));
            if (!m.find()) return invalid("SYSTEM.CNF does not contain a valid BOOT2 entry.");
            String boot = normalize(m.group(1));
            if (!boot.equalsIgnoreCase(EXPECTED_EXE))
                return invalid("Unsupported PlayStation 2 build (" + boot + ").");

            Rec exe = find(entries, EXPECTED_EXE);
            if (exe == null) return invalid(EXPECTED_EXE + " was not found in the ISO root.");
            long exeOff = exe.extent * SECTOR;
            byte[] magic = iso.tryRead(exeOff, 4);
            if (magic == null || magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F')
                return invalid(EXPECTED_EXE + " is not a valid ELF executable.");
            return new Result(true, "Sly 1 NTSC detected.", exeOff, exe.length);
        } catch (IOException | RuntimeException e) {
            return invalid("The ISO could not be opened or read.");
        }
    }

    private static Rec find(List<Rec> entries, String name) {
        for (Rec r : entries) if (!r.dir && r.name.equalsIgnoreCase(name)) return r;
        return null;
    }

    private static List<Rec> readDirectory(byte[] data) {
        List<Rec> list = new ArrayList<>();
        int off = 0;
        while (off < data.length) {
            int len = data[off] & 0xFF;
            if (len == 0) { off = ((off / SECTOR) + 1) * SECTOR; continue; }
            if (off + len > data.length) break;
            Rec r = readRecord(data, off);
            if (r != null && !r.name.equals(".") && !r.name.equals("..")) list.add(r);
            off += len;
        }
        return list;
    }

    private static Rec readRecord(byte[] d, int off) {
        if (off < 0 || off + 34 > d.length) return null;
        int recLen = d[off] & 0xFF;
        int nameLen = d[off + 32] & 0xFF;
        if (recLen < 34 || off + recLen > d.length || off + 33 + nameLen > d.length) return null;
        long extent = Bytes.le32(d, off + 2);
        long length = Bytes.le32(d, off + 10);
        int flags = d[off + 25] & 0xFF;
        String name;
        if (nameLen == 1 && d[off + 33] == 0) name = ".";
        else if (nameLen == 1 && d[off + 33] == 1) name = "..";
        else name = normalize(new String(d, off + 33, nameLen, StandardCharsets.US_ASCII));
        return new Rec(extent, Bytes.toInt(length), (flags & 2) != 0, name);
    }

    private static String normalize(String name) {
        String n = name.trim().replace('/', '\\');
        int sep = n.lastIndexOf('\\');
        if (sep >= 0) n = n.substring(sep + 1);
        int ver = n.indexOf(';');
        if (ver >= 0) n = n.substring(0, ver);
        return n.trim();
    }
}
