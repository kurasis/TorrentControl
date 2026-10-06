// Developer-only audit of BiglyBT's parser and hash routines. No Core/GUI/swarm is started.
import com.biglybt.core.torrent.*;
import com.biglybt.core.util.SHA1Hasher;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

class BiglyProof {
    static Path path(TOTorrent torrent, TOTorrentFile file, Path parent) throws IOException {
        Path root = parent.resolve(new String(torrent.getName(), StandardCharsets.UTF_8));
        Path result = root;
        if (!torrent.isSimpleTorrent()) {
            for (byte[] component : file.getPathComponents()) {
                String name = new String(component, StandardCharsets.UTF_8);
                if (name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\"))
                    throw new IOException("unsafe component");
                result = result.resolve(name);
            }
        }
        if (!result.normalize().startsWith(parent)) throw new IOException("unsafe path");
        return result;
    }

    static boolean v1(TOTorrent torrent, Path parent) throws Exception {
        byte[][] hashes = torrent.getPieces();
        int pieceLength = Math.toIntExact(torrent.getPieceLength());
        if (pieceLength < 16384 || pieceLength > 16 * 1024 * 1024) throw new IOException("audit piece limit");
        byte[] piece = new byte[pieceLength];
        int used = 0, index = 0;
        boolean valid = true;
        for (TOTorrentFile file : torrent.getFiles()) {
            Path payload = path(torrent, file, parent);
            if (!file.isPadFile() && (!Files.isRegularFile(payload) || Files.size(payload) != file.getLength())) return false;
            try (InputStream stream = file.isPadFile() ? InputStream.nullInputStream() : Files.newInputStream(payload)) {
                long remaining = file.getLength();
                while (remaining > 0) {
                    int count = (int)Math.min(remaining, pieceLength - used);
                    if (file.isPadFile()) Arrays.fill(piece, used, used + count, (byte)0);
                    else if (stream.readNBytes(piece, used, count) != count) return false;
                    remaining -= count;
                    used += count;
                    if (used == pieceLength) {
                        valid &= index < hashes.length && Arrays.equals(hashes[index], new SHA1Hasher().calculateHash(piece));
                        index++;
                        used = 0;
                    }
                }
            }
        }
        if (used > 0) {
            valid &= index < hashes.length && Arrays.equals(hashes[index], new SHA1Hasher().calculateHash(Arrays.copyOf(piece, used)));
            index++;
        }
        return valid && index == hashes.length;
    }

    static boolean v2(TOTorrent torrent, Path parent) throws Exception {
        boolean valid = true;
        Map<?, ?> layers = torrent.getAdditionalMapProperty("piece layers");
        // The stock hybrid root fixup assumes identical v1/v2 file counts. Libtorrent
        // retains a trailing v1 pad entry; inspect the original v2 tree independently.
        Class<?> impl = Class.forName("com.biglybt.core.torrent.impl.TOTorrentImpl");
        var properties = impl.getDeclaredMethod("getAdditionalInfoProperties");
        properties.setAccessible(true);
        Map<?, ?> info = (Map<?, ?>)properties.invoke(torrent);
        Class<?> creator = Class.forName("com.biglybt.core.torrent.impl.TOTorrentCreateV2Impl");
        var build = creator.getDeclaredMethod("lashUpV2Files", impl, List.class, LinkedList.class,
                                               Map.class, long.class, long[].class, long[].class);
        build.setAccessible(true);
        List<TOTorrentFile> files = new ArrayList<>();
        build.invoke(null, torrent, files, new LinkedList<byte[]>(), info.get("file tree"),
                     torrent.getPieceLength(), new long[]{0}, new long[]{0, 0});
        for (TOTorrentFile file : files) {
            if (file.isPadFile()) continue;
            Path payload = path(torrent, file, parent);
            if (!Files.isRegularFile(payload) || Files.size(payload) != file.getLength()) { valid = false; continue; }
            if (file.getLength() == 0) continue;
            byte[] expected = file.getRootHash();
            valid &= expected != null && Arrays.equals(expected, TOTorrentFactory.getV2RootHash(payload.toFile()));
            if (file.getLength() > torrent.getPieceLength()) {
                Object layer = expected == null || layers == null ? null : layers.get(new String(expected, StandardCharsets.ISO_8859_1));
                if (!(layer instanceof byte[])) { valid = false; continue; }
                // Pinned BiglyBT internal API: use the client's Merkle validation, not an audit reimplementation.
                TOTorrentFileHashTree tree = file.getHashTree();
                var method = tree.getClass().getDeclaredMethod("addPieceLayer", byte[].class);
                method.setAccessible(true);
                try {
                    List<?> hashes = (List<?>)method.invoke(tree, layer);
                    valid &= hashes.size() == (file.getLength() + torrent.getPieceLength() - 1) / torrent.getPieceLength();
                } catch (java.lang.reflect.InvocationTargetException error) {
                    if (!(error.getCause() instanceof TOTorrentException)) throw error;
                    valid = false;
                }
            }
        }
        return valid;
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("TORRENT PAYLOAD_PARENT");
        PrintStream output = System.out;
        System.setOut(System.err); // BiglyBT diagnostics must not corrupt the JSON result.
        TOTorrent torrent = TOTorrentFactory.deserialiseFromBEncodedFile(new File(args[0]));
        Path parent = Path.of(args[1]).toAbsolutePath().normalize();
        int type = torrent.getTorrentType();
        boolean hasV1 = type == TOTorrent.TT_V1 || type == TOTorrent.TT_V1_V2;
        boolean hasV2 = type == TOTorrent.TT_V2 || type == TOTorrent.TT_V1_V2;
        boolean verifiedV1 = !hasV1 || v1(torrent, parent);
        boolean verifiedV2 = !hasV2 || v2(torrent, parent);
        String hashes = (hasV1 ? ",\"infohashV1\":\"" + HexFormat.of().formatHex(torrent.getFullHash(TOTorrent.TT_V1)) + "\"" : "")
            + (hasV2 ? ",\"infohashV2\":\"" + HexFormat.of().formatHex(torrent.getFullHash(TOTorrent.TT_V2)) + "\"" : "");
        output.println("{\"engine\":\"BiglyBT 4.1.0.0\",\"scope\":\"parser-and-hash-routines\",\"network\":\"no-core-started\","
            + "\"hasV1\":" + hasV1 + ",\"hasV2\":" + hasV2 + ",\"verifiedV1\":" + verifiedV1
            + ",\"verifiedV2\":" + verifiedV2 + ",\"verified\":" + (verifiedV1 && verifiedV2) + hashes + "}");
        System.exit(verifiedV1 && verifiedV2 ? 0 : 5);
    }
}
