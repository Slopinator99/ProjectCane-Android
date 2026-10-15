package org.projectcane.cane.extract;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Port of Sly1MediaExtractionService: music, sound banks, commentary, dialog, cutscenes, ambient, prompt. */
public final class MediaExtractor {
    private static final int SECTOR = 0x800;
    private static final long ELF_VA = 0x00100000L, ELF_FILE = 0x1000L;
    private static final long LEVEL_TABLE_VA = 0x00247AF0L, SOUND_TABLE_VA = 0x00244F40L, COMMENTARY_TABLE_VA = 0x00248748L;
    private static final long AMBIENT_TABLE_VA = 0x002482D8L, CUTSCENE_TABLE_VA = 0x00248490L, PROMPT_VA = 0x0026A118L;
    private static final long SPLASH_SECTOR = 0xC8F03L, SPLASH_SIZE = 0xAB4004L;
    private static final String[] BANK_NAMES = {"Common", "Intro", "Underwater", "Muggshot", "Voodoo", "Snow", "Clockwerk"};

    private static final class Mus {
        final String level; final int id; final long sector, size;
        Mus(String l, int i, long s, long z) { level = l; id = i; sector = s; size = z; }
    }
    private static Mus m(String l, int id, long s, long z) { return new Mus(l, id, s, z); }

    private static final Mus[] MUSIC = {
        m("Paris, France",0x55,0xC8E0E,0x7A6A0),m("The Hideout",0x43,0xD3411,0x6F7A0),m("A Cunning Disguise",0x47,0xC4602,0x72324),
        m("A Stealthy Approach",0x51,0xD44ED,0x745F8),m("High Class Heist",0x59,0xCDE0B,0x74674),m("Into the Machine",0x3F,0xC839D,0x7B414),
        m("Prowling the Grounds",0x44,0xBFA16,0x746D4),m("The Eye of the Storm",0x57,0xC354B,0x7A0C4),m("The Fire Down Below",0x46,0xC130C,0x775E0),
        m("The Gunboat Graveyard",0x42,0xCAC3D,0x70D4C),m("Treasure in the Depths",0x3E,0xD4E9D,0x748F8),m("A Rocky Start",0x50,0xBC6B4,0x64A34),
        m("At the Dog Track",0x4E,0xCFBE5,0x726F8),m("Back Alley Heist",0x4C,0xC1B99,0x77978),m("Boneyard Casino",0x49,0xCF341,0x71A18),
        m("Last Call",0x48,0xC02DE,0x731B8),m("Muggshot's Turf",0x40,0xCE9BB,0x5DB18),m("Murray's Big Gamble",0x4A,0xCD592,0x767B8),
        m("Straight to the Top",0x4C,0xD04EF,0x77978),m("Two to Tango",0x4D,0xD5A8B,0x788FC),m("A Deadly Dance",0x68,0xBE35F,0x771E8),
        m("A Ghastly Voyage",0x65,0xC6338,0x76D9C),m("A Grave Undertaking",0x55,0xCCD68,0x7A6A0),m("Descent into Danger",0x64,0xC25C6,0x6EBB8),
        m("Down Home Cooking",0x62,0xC50BB,0x73978),m("Piranha Lake",0x67,0xD4AFA,0x73978),m("The Dread Swamp Path",0x60,0xC6D86,0x76A60),
        m("The Lair of the Beast",0x64,0xCB6A5,0x6EBB8),m("The Swamp's Dark Center",0x64,0xD1852,0x6EBB8),m("A Desperate Race",0x4E,0xCBD9B,0x726F8),
        m("A Perilous Ascent",0x53,0xD2424,0x773CC),m("Duel by the Dragon",0x5B,0xBD2A0,0x6DA90),m("Flame Fu!",0x56,0xC4B48,0x6EA6C),
        m("Flaming Temple of Flame",0x54,0xBEF0F,0x7737C),m("Inside the Stronghold",0x5B,0xC7870,0x6DA90),m("Rapid Fire Assault",0x5D,0xC2F89,0x6C2BC),
        m("The King of the Hill",0x5C,0xD2D42,0x78064),m("The Unseen Foe",0x44,0xC5A33,0x746D4),m("A Daring Rescue",0x38,0xBD8B1,0x788F8),
        m("A Hazardous Path",0x35,0xC3CB5,0x77C7C),m("A Strange Reunion",0x34,0xC0AAC,0x95774),m("A Temporary Truce",0x39,0xD0BD7,0x74DAC),
        m("Bentley Comes Through",0x36,0xD50F1,0x8C528),m("Burning Rubber",0x3A,0xCC1CA,0x77614),m("Sinking Peril",0x37,0xD3A9E,0x77F08)
    };

    private static final class Desc {
        final long sector, size, identity;
        Desc(long s, long z, long i) { sector = s; size = z; identity = i; }
    }
    private static final class Dialog {
        final long levelId, sector, size, identity; final String source;
        Dialog(long l, long s, long z, long i, String src) { levelId = l; sector = s; size = z; identity = i; source = src; }
    }

    private MediaExtractor() {}

    /** @param dialogTable contents of DialogAudioTable.inc */
    public static void extract(IsoSource iso, File output, long exe, String dialogTable,
                               ProgressListener pl, double base, double span) throws IOException {
        List<Dialog> dialogs = loadDialogTable(dialogTable);
        Set<Integer> distinct = new HashSet<>();
        for (Mus x : MUSIC) distinct.add(x.id);
        final int total = distinct.size() + 7 + 33 + dialogs.size() + 18 + 6 + 1;
        int done = 0;

        File worlds = new File(output, "Worlds");
        File sounds = new File(worlds, "Sounds");
        File musicDir = new File(worlds, "Music");
        Fs.mkdirs(musicDir);
        List<String> musicMap = new ArrayList<>();
        for (Mus x : MUSIC)
            musicMap.add(String.format(Locale.ROOT, "%s | MUSID 0x%X | sector 0x%X | bytes 0x%X", x.level, x.id, x.sector, x.size));
        Fs.writeLines(new File(musicDir, "MusicMap.txt"), musicMap);

        Set<Integer> seen = new HashSet<>();
        for (Mus x : MUSIC) {
            if (!seen.add(x.id)) continue;
            String name = String.format(Locale.ROOT, "MUSID_%02X.mus", x.id);
            report(pl, base, span, "Music / " + name, done, total);
            File dst = new File(musicDir, name);
            if (!Fs.done(dst)) Fs.writeAtomic(dst, iso.read(x.sector * SECTOR, Bytes.toInt(x.size)));
            done++;
        }

        List<Desc> banks = readDescriptors(iso, exe, SOUND_TABLE_VA, 7);
        File bankDir = new File(worlds, "Sound Banks");
        Fs.mkdirs(bankDir);
        for (int i = 0; i < banks.size(); i++) {
            report(pl, base, span, "Sound Banks / " + BANK_NAMES[i] + ".bnk", done, total);
            copyDescriptor(iso, banks.get(i), new File(bankDir, BANK_NAMES[i] + ".bnk"), false);
            done++;
        }

        byte[] comm = iso.read(vaOffset(exe, COMMENTARY_TABLE_VA), 33 * 44);
        File commDir = new File(sounds, "Commentary");
        Fs.mkdirs(commDir);
        List<String> commMap = new ArrayList<>();
        for (int i = 0; i < 33; i++) {
            int o = i * 44;
            long levelId = Bytes.le32(comm, o);
            float duration = Float.intBitsToFloat((int) Bytes.le32(comm, o + 4));
            Desc d = descriptor(comm, o + 8);
            long speakerVa = Bytes.le32(comm, o + 40);
            if (d.sector == 0 || d.size == 0) { done++; continue; }
            String name = String.format(Locale.ROOT, "VAG_%04X_%06X.wav", d.identity, d.sector);
            report(pl, base, span, "Sounds / Commentary / " + name, done, total);
            File dst = new File(commDir, name);
            if (!Fs.done(dst)) {
                byte[] vag = iso.read(d.sector * SECTOR, Bytes.toInt(d.size));
                Fs.writeAtomic(dst, AudioCodec.decodeVag(vag));
            }
            String speakers = readExeString(iso, exe, speakerVa);
            if (speakers == null) speakers = "Unknown";
            speakers = speakers.replace("\r\n", "\n");
            commMap.add(String.format(Locale.ROOT,
                    "%s | levelId 0x%X | duration %s | identity 0x%X | sector 0x%X | bytes 0x%X | speakers %s",
                    name, levelId, floatToString(duration), d.identity, d.sector, d.size, speakers));
            done++;
        }
        Fs.writeLines(new File(commDir, "CommentaryMap.txt"), commMap);

        Map<Long, String> levelNames = readLevelNames(iso, exe);
        Map<File, List<String>> dialogMaps = new LinkedHashMap<>();
        for (Dialog d : dialogs) {
            if (pl.isCancelled()) throw new IOException("Cancelled.");
            String levelName;
            if (d.levelId == 3) levelName = "Paris";
            else if (d.levelId == 4) levelName = "The Hideout";
            else levelName = levelNames.containsKey(d.levelId) ? levelNames.get(d.levelId) : "Level_" + d.levelId;
            String episode = LevelExtractor.episode(d.levelId);
            File dir = new File(new File(new File(worlds, episode), levelName), "Dialog");
            Fs.mkdirs(dir);
            String name = String.format(Locale.ROOT, "VAG_%04X_%06X.wav", d.identity, d.sector);
            report(pl, base, span, "Worlds / " + episode + " / " + levelName + " / Dialog / " + name, done, total);
            long size = d.size;
            File dst = new File(dir, name);
            if (size == 0) {
                byte[] header = iso.read(d.sector * SECTOR, 0x30);
                size = Bytes.be32(header, 0x0C) + 0x30;
            }
            if (!Fs.done(dst)) {
                byte[] vag = iso.read(d.sector * SECTOR, Bytes.toInt(size));
                Fs.writeAtomic(dst, AudioCodec.decodeVag(vag));
            }
            dialogMaps.computeIfAbsent(new File(dir, "DialogMap.txt"), k -> new ArrayList<>()).add(String.format(Locale.ROOT,
                    "%s | levelId 0x%X | identity 0x%X | sector 0x%X | bytes 0x%X | source %s",
                    name, d.levelId, d.identity, d.sector, size, d.source));
            done++;
        }
        for (Map.Entry<File, List<String>> e : dialogMaps.entrySet()) Fs.writeLines(e.getKey(), e.getValue());

        List<Desc> cutscenes = readDescriptors(iso, exe, CUTSCENE_TABLE_VA, 17);
        File cutDir = new File(output, "Cutscenes");
        Fs.mkdirs(cutDir);
        for (int i = 0; i < 18; i++) {
            if (pl.isCancelled()) throw new IOException("Cancelled.");
            Desc d = i == 17 ? new Desc(SPLASH_SECTOR, SPLASH_SIZE, 0x344) : cutscenes.get(i);
            int outIndex = i == 17 ? 17 : i == 0 ? 16 : i - 1;
            String stem = String.format(Locale.ROOT, "Cutscene_%02d", outIndex);
            report(pl, base, span, "Cutscenes / " + stem, done, total);
            File v = new File(cutDir, stem + ".m2v"), a = new File(cutDir, stem + ".wav");
            if (!(Fs.done(v) && Fs.done(a))) {
                // Stream the video to disk: the largest cutscene is ~350 MB, more than the Java heap allows in memory.
                File vTmp = new File(v.getPath() + ".part"), aTmp = new File(a.getPath() + ".part");
                try (OutputStream vos = new BufferedOutputStream(new FileOutputStream(vTmp), 1 << 16);
                     OutputStream aos = new BufferedOutputStream(new FileOutputStream(aTmp), 1 << 16)) {
                    AudioCodec.demuxPss(iso, d.sector * SECTOR, Bytes.toInt(d.size), vos, aos);
                } catch (IOException | RuntimeException e) {
                    vTmp.delete();
                    aTmp.delete();
                    throw e;
                }
                Fs.commit(vTmp, v);
                Fs.commit(aTmp, a);
            }
            done++;
        }

        List<Desc> ambient = readDescriptors(iso, exe, AMBIENT_TABLE_VA, 6);
        File ambDir = new File(sounds, "JLO Ambient");
        Fs.mkdirs(ambDir);
        for (int i = 0; i < ambient.size(); i++) {
            String name = "JloAmbient_" + (i + 1) + ".wav";
            report(pl, base, span, "Sounds / JLO Ambient / " + name, done, total);
            copyDescriptor(iso, ambient.get(i), new File(ambDir, name), true);
            done++;
        }
        Desc prompt = readDescriptors(iso, exe, PROMPT_VA, 1).get(0);
        File promptDir = new File(sounds, "Prompt");
        Fs.mkdirs(promptDir);
        report(pl, base, span, "Sounds / Prompt / PromptPrk18.wav", done, total);
        copyDescriptor(iso, prompt, new File(promptDir, "PromptPrk18.wav"), true);
        done++;
        report(pl, base, span, "Media extraction complete", total, total);
    }

    /** Mirrors C# float.ToString(InvariantCulture) closely enough for the %f parse in sound.cpp. */
    static String floatToString(float f) {
        if (f == Math.rint(f) && Math.abs(f) < 1e7f) return Long.toString((long) f);
        return Float.toString(f);
    }

    private static void report(ProgressListener pl, double base, double span, String item, int done, int total) {
        pl.onProgress(item, base + span * done / (double) total);
    }

    private static List<Desc> readDescriptors(IsoSource iso, long exe, long va, int count) throws IOException {
        byte[] b = iso.read(vaOffset(exe, va), count * 32);
        List<Desc> r = new ArrayList<>(count);
        for (int i = 0; i < count; i++) r.add(descriptor(b, i * 32));
        return r;
    }

    private static Desc descriptor(byte[] d, int o) {
        return new Desc(Bytes.le32(d, o) ^ Bytes.le32(d, o + 20),
                Bytes.le32(d, o + 4) ^ Bytes.le32(d, o + 28),
                Bytes.le32(d, o + 8) ^ Bytes.le32(d, o + 24));
    }

    private static void copyDescriptor(IsoSource iso, Desc d, File dst, boolean wav) throws IOException {
        if (d.sector == 0 || d.size == 0) throw new IOException("Invalid media descriptor.");
        if (Fs.done(dst)) return;
        byte[] data = iso.read(d.sector * SECTOR, Bytes.toInt(d.size));
        Fs.writeAtomic(dst, wav ? AudioCodec.decodeVag(data) : data);
    }

    private static long vaOffset(long exe, long va) { return exe + (va - ELF_VA) + ELF_FILE; }

    private static String readExeString(IsoSource iso, long exe, long va) throws IOException {
        if (va < ELF_VA) return null;
        long off = vaOffset(exe, va);
        if (off < 0 || off >= iso.length()) return null;
        byte[] b = iso.read(off, (int) Math.min(256, iso.length() - off));
        for (int i = 0; i < b.length; i++)
            if (b[i] == 0) return new String(b, 0, i, StandardCharsets.US_ASCII);
        return null;
    }

    private static Map<Long, String> readLevelNames(IsoSource iso, long exe) throws IOException {
        byte[] table = iso.read(vaOffset(exe, LEVEL_TABLE_VA), 46 * 44);
        Map<Long, String> r = new HashMap<>();
        for (int i = 0; i < 46; i++) {
            int o = i * 44;
            long id = Bytes.le32(table, o + 32);
            String name = readExeString(iso, exe, Bytes.le32(table, o + 36));
            if (name != null && !name.trim().isEmpty() && !r.containsKey(id)) r.put(id, LevelExtractor.sanitize(name));
        }
        return r;
    }

    private static final Pattern DIALOG = Pattern.compile(
            "\\{\\s*(?:0x)?([0-9A-F]+),\\s*(?:0x)?([0-9A-F]+),\\s*(?:0x)?([0-9A-F]+),\\s*(?:0x)?([0-9A-F]+),\\s*DialogAudioSource::(\\w+)\\s*\\}",
            Pattern.CASE_INSENSITIVE);

    static List<Dialog> loadDialogTable(String text) {
        List<Dialog> r = new ArrayList<>();
        Matcher m = DIALOG.matcher(text);
        while (m.find())
            r.add(new Dialog(Long.parseLong(m.group(1), 16), Long.parseLong(m.group(2), 16),
                    Long.parseLong(m.group(3), 16), Long.parseLong(m.group(4), 16), m.group(5)));
        return r;
    }

    public static String readAll(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8))) {
            char[] buf = new char[8192];
            int n;
            while ((n = br.read(buf)) > 0) sb.append(buf, 0, n);
        }
        return sb.toString();
    }
}
