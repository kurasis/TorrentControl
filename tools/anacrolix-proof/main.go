// Developer-only compatibility check using an independent client implementation.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"time"

	g "github.com/anacrolix/generics"
	"github.com/anacrolix/torrent"
	"github.com/anacrolix/torrent/metainfo"
	"github.com/anacrolix/torrent/storage"
)

func run() (map[string]any, error) {
	if len(os.Args) != 3 {
		return nil, errors.New("usage: anacrolix-proof TORRENT PAYLOAD_PARENT")
	}
	mi, err := metainfo.LoadFromFile(os.Args[1])
	if err != nil {
		return nil, err
	}
	info, err := mi.UnmarshalInfo()
	if err != nil {
		return nil, err
	}
	if info.HasV2() {
		if err := metainfo.ValidatePieceLayers(mi.PieceLayers, &info.FileTree, info.PieceLength); err != nil {
			return nil, err
		}
	}
	if !info.HasV1() && info.HasV2() {
		return map[string]any{"engine": "anacrolix/torrent v1.61.0", "supported": false,
			"reason":            "pure-v2 AddTorrentOpt requires a nonzero v1 lookup ID; stock client import is not verified",
			"metadataValidated": true, "network": "disabled"}, nil
	}
	cfg := torrent.NewDefaultClientConfig()
	cfg.DisableTrackers = true
	cfg.NoDHT = true
	cfg.DisablePEX = true
	cfg.NoUpload = true
	cfg.DisableTCP = true
	cfg.DisableUTP = true
	cfg.DisableWebtorrent = true
	cfg.DisableWebseeds = true
	cfg.NoDefaultPortForwarding = true
	cfg.DialForPeerConns = false
	cfg.ListenPort = 0
	deny := func(context.Context, string, string) (net.Conn, error) {
		return nil, errors.New("network disabled in compatibility proof")
	}
	cfg.HTTPDialContext = deny
	cfg.TrackerDialContext = deny
	cfg.DefaultStorage = storage.NewFileOpts(storage.NewFileClientOpts{
		ClientBaseDir: os.Args[2], PieceCompletion: storage.NewMapPieceCompletion(), UsePartFiles: g.Some(false),
	})
	defer cfg.DefaultStorage.(storage.ClientImplCloser).Close()
	client, err := torrent.NewClient(cfg)
	if err != nil {
		return nil, err
	}
	defer client.Close()
	spec := torrent.TorrentSpecFromMetaInfo(mi)
	spec.DisallowDataDownload = true
	spec.DisallowDataUpload = true
	t, _, err := client.AddTorrentSpec(spec)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := t.VerifyDataContext(ctx); err != nil {
		return nil, err
	}
	h2 := sha256.Sum256(mi.InfoBytes)
	return map[string]any{"engine": "anacrolix/torrent v1.61.0", "supported": true, "verified": t.BytesMissing() == 0,
		"bytesMissing": t.BytesMissing(), "pieces": t.NumPieces(), "hasV1": info.HasV1(), "hasV2": info.HasV2(),
		"infohashV1": mi.HashInfoBytes().HexString(), "infohashV2": hex.EncodeToString(h2[:]), "network": "disabled"}, nil
}

func main() {
	result, err := run()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	if err := json.NewEncoder(os.Stdout).Encode(result); err != nil {
		os.Exit(2)
	}
	if result["supported"] == false {
		os.Exit(6)
	}
	if result["verified"] != true {
		os.Exit(5)
	}
}
