# Windows SMB cancellation and application shutdown

`tests/windows/run-smb-faults.ps1` exercises the production Windows UNC adapter
and the actual Win32/WebView2 application against Samba in a separate Linux
guest kernel. QEMU uses TCG; neither Hyper-V nor nested virtualization is
required. A task-owned TAP adapter connects Windows `192.168.240.1/30` to the
guest `192.168.240.2/30`. SMB uses TCP port 445 and SMB 3.0–3.1.1. This is a
controlled VM peer on one Windows Server 2022 CI machine, not a physical NAS
or evidence for every Windows 10/11 version and storage driver.

The fixture pins QEMU, Alpine virt 3.22.6 and TAP-Windows 9.27.0 downloads to
upstream SHA-512/SHA-256 values. Windows must validate Microsoft signatures on
the driver catalog and installation tool. Its SMB signing, authentication and
guest policies are unchanged. A random fixture-only account authenticates a
temporary connection with `WNetAddConnection2W`; no user credentials are used.
The independent serial console disables echo, and its log redacts the fixture
password. Credentials never appear in command-line arguments or report JSON.

The host fetches only named official Alpine v3.22 HTTPS package assets through
a restricted local cache. The guest verifies the signed APK index and package
signatures using the keys bundled with the pinned ISO. TLS verification and
APK signature checks remain enabled. The guest's slirp NIC serves package
setup; its second, private TAP NIC serves Windows SMB. Before faults, a real
64 MiB SMB read inside the guest must match the source SHA-256.

## Scenarios and assertions

For each of v1, v2 and hybrid, the ordinary headless AppService workflow creates
a baseline torrent over UNC. An independent Python implementation verifies
its payload hashes. Fifteen cases then cover:

- Headless cancellation while a production read is pending.
- Headless cancellation while a production source open is pending.
- Server termination during a pending read, which must fail the job.
- Actual GUI `WM_CLOSE` while a production read is pending.
- Actual GUI `WM_CLOSE` while a production source open is pending.

The shared developer probe coordinates a one-shot gate immediately before the
ordinary file adapter. Samba's process group is suspended, then the gate is
released. A gate alone never counts as pending I/O: the production operation
must remain active for 250 ms after release with zero bytes accepted. Control
replies and a native window message acknowledgement remain bounded to two
seconds. The report records the observed cancellation and shutdown latency.

Pending opens are synchronous and retain their worker until Windows returns.
Reads request cancellation through the existing production adapter; Windows
may keep the reader owned until the driver and handle close settle. The
fixture resumes the server and requires a terminal result and real join with
zero readers and no pending native operation. It makes no universal two-second
promise for OS completion or shutdown when a peer stays unavailable forever.

GUI cases start the bundled frontend in the actual WebView2 host, use normal
scan/validate/create operations, then post `WM_CLOSE` to the task-owned HWND.
The existing self-test mode bypasses the confirmation dialog. A shutdown
marker is written only after AppService destruction joins its workers. Exit
must be normal, and tracked WebView2 children must disappear. Killing a process
for failure cleanup cannot count as a passing shutdown case.

Every case preserves the previous torrent bytes, leaves no temporary torrent,
recovers the existing SMB connection, and reproduces the baseline on a healthy
retry. The source SHA-256 must stay unchanged. Cleanup removes the owned VM,
temporary SMB connection and exact TAP device identity; it never removes other
adapters, changes physical interfaces, or uninstalls a global driver package.

## Reproduction

Run on a disposable, administrator-capable Windows machine with Python,
7-Zip and the built development binaries. Install the existing
`tests/performance/requirements.txt`, then:

```powershell
./tests/windows/run-smb-faults.ps1 -DisposableRunner
```

The normal Windows CI job runs this before fresh process-memory comparisons.
Its `windows-smb-faults` artifact retains the summary JSON, per-case probe and
process evidence, torrents, and redacted guest/application logs. Downloaded
executables, driver archives and APK package bytes are excluded.

The helper's Linux local-control mode checks guest startup, authenticated
payload transfer and Samba stop/resume/termination only. It cannot establish
Windows redirector behavior and is not a substitute for the Windows CI run.

Physical remote NAS fault testing, indefinitely unavailable sources, other
driver stacks and clean desktop Windows 10/11 installation remain release
gates. See [remaining I/O limits](cancellable-payload-io.md) and
[the process-memory package](process-memory-smb.md) for the earlier evidence.
