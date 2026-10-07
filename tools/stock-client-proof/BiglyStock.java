// Developer driver: the unmodified BiglyBT Core/GlobalManager owns import and checking.
// No parser fixups, reflection, file remapping or custom hash routines.
import com.biglybt.core.Core;
import com.biglybt.core.CoreFactory;
import com.biglybt.core.config.COConfigurationManager;
import com.biglybt.core.download.*;
import com.biglybt.core.disk.DiskManagerFileInfo;
import com.biglybt.core.torrent.TOTorrent;
import com.biglybt.pif.PluginManager;
import java.io.*;
import java.nio.file.*;
import java.util.*;

class BiglyStock {
    static String string(String value) {
        if (value == null) return "null";
        return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"")
            .replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t") + "\"";
    }
    static String hash(TOTorrent torrent, int type) throws Exception {
        byte[] value = torrent.getFullHash(type);
        return value == null ? "null" : string(HexFormat.of().formatHex(value));
    }
    static String status(DownloadManager download) {
        var stats = download.getStats();
        var files = new ArrayList<String>();
        for (DiskManagerFileInfo file : download.getDiskManagerFileInfoSet().getFiles()) {
            files.add("{\"path\":" + string(file.getFile(true).toString())
                + ",\"length\":" + file.getLength() + ",\"downloaded\":" + file.getDownloaded()
                + ",\"padding\":" + file.getTorrentFile().isPadFile() + "}");
        }
        return "{\"complete\":" + (download.getState() != DownloadManager.STATE_ERROR && download.isDownloadComplete(true))
            + ",\"completedPermille\":" + stats.getDownloadCompleted(false)
            + ",\"state\":" + download.getState()
            + ",\"error\":" + string(download.getErrorDetails())
            + ",\"receivedBytes\":" + stats.getTotalDataBytesReceived()
            + ",\"sentBytes\":" + stats.getTotalDataBytesSent()
            + ",\"files\":[" + String.join(",", files) + "]}";
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new IllegalArgumentException("PROFILE TORRENT PAYLOAD_PARENT");
        Path profile = Path.of(args[0]);
        Files.createDirectories(profile.resolve("torrents"));
        System.setProperty("azureus.config.path", profile.toString());
        System.setProperty("azureus.loadplugins", "0");
        PrintStream output = System.out;
        System.setOut(System.err);
        COConfigurationManager.initialise();
        COConfigurationManager.setParameter("TCP.Listen.Port.Enable", false);
        COConfigurationManager.setParameter("UDP.Listen.Port.Enable", false);
        COConfigurationManager.setParameter("UDP.NonData.Listen.Port.Enable", false);
        COConfigurationManager.setParameter("update.start", false);
        COConfigurationManager.setParameter("update.periodic", false);
        COConfigurationManager.setParameter("Save Torrent Files", true);
        COConfigurationManager.setParameter("General_sDefaultTorrent_Directory", profile.resolve("torrents").toString());
        var plugins = PluginManager.getDefaults();
        for (String plugin : plugins.getDefaultPlugins()) plugins.setDefaultPluginEnabled(plugin, false);
        Core core = CoreFactory.create();
        try {
            core.start();
            var download = core.getGlobalManager().addDownloadManager(args[1], null, args[2],
                DownloadManager.STATE_STOPPED, true, false, new DownloadManagerInitialisationAdapter() {
                    public int getActions() { return ACT_NONE; }
                    public void initialised(DownloadManager manager, boolean seeding) {
                        manager.getDownloadState().setNetworks(new String[0]);
                        manager.getDownloadState().setPeerSources(new String[0]);
                    }
                });
            if (download == null || download.getTorrent() == null) throw new IOException("Download manager import failed");
            TOTorrent torrent = download.getTorrent();
            var files = new ArrayList<String>();
            for (DiskManagerFileInfo file : download.getDiskManagerFileInfoSet().getFiles()) {
                files.add("{\"path\":" + string(file.getFile(true).toString())
                    + ",\"length\":" + file.getLength()
                    + ",\"padding\":" + file.getTorrentFile().isPadFile() + "}");
            }
            output.println("{\"ready\":true,\"client\":\"BiglyBT 4.1.0.0\",\"infohashV1\":"
                + hash(torrent, TOTorrent.TT_V1) + ",\"infohashV2\":" + hash(torrent, TOTorrent.TT_V2)
                + ",\"files\":[" + String.join(",", files) + "]}");
            var commands = new BufferedReader(new InputStreamReader(System.in));
            for (String command; (command = commands.readLine()) != null;) {
                if (command.equals("quit")) break;
                if (!command.equals("check")) throw new IllegalArgumentException("Unknown command");
                if (!download.canForceRecheck()) throw new IOException("Client cannot force recheck in state " + download.getState());
                download.forceRecheck();
                long deadline = System.nanoTime() + 120_000_000_000L;
                while (download.isForceRechecking()) {
                    if (System.nanoTime() > deadline) throw new IOException("Native recheck deadline exceeded");
                    Thread.sleep(20);
                }
                output.println(status(download));
            }
        } finally {
            core.stop();
        }
        output.println("{\"normalExit\":true}");
    }
}
