# Disposable-VM capture acceptance

This is a separate elevated native test, not `urnetworkd selftest`, and is not
part of the shipping solution or build. Use only the disposable Windows VM in
`build/BUILD-PLATFORMS.md`; never run it on a workstation or an installed client.
It temporarily changes routes, adapter DNS, and machine-wide WFP filtering.

After staging these test files and the current `app/src` tree in the VM, reuse
the existing MSVC/Windows SDK and project-pinned Wintun dependency:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\build\urnetwork\windows\app\tools\run-capture-native-tests.ps1 -DisposableVm -Platform ARM64
```

Use `-BuildOnly` for compile-only evidence. `-WintunDirectory` can select an
already staged dependency; there is no fetch/install-service/account step.
The output directory must be new and retains source hashes, build/test logs,
timestamps, and the terminal native exit. It also retains a bounded raw network
and owned-policy baseline, each child's actual exit before the cleanup verdict,
and per-case final snapshots with field-by-field difference counts. These raw
snapshots contain guest network configuration and must remain restricted local
evidence; console messages contain only fixed labels and counts. Preserve the
directory outside the VM before
discarding the overlay. Do not modify the original build VM or base image while
another build owns it; the build script normally shuts its guest down on exit.

The executable refuses elevation/ownership ambiguity, an installed `urnetworkd`
service, an existing tunnel adapter, or existing product WFP objects. It uses a
test-only adapter GUID, the production Wintun, NetworkConfig and WfpPolicy
implementations, and the production ApplyCapture transaction. Nineteen bounded
child cases inspect actual route prefixes, DNS/address assignments, physical
interface settings and the BFE filter conditions, not only owner booleans:

- No proof and expired/superseded/cancelled/network-invalidated tickets do not
  capture. Valid injected proof applies the real IPv4 and dual-stack state.
- Failure after each transaction stage, an exception, invalid network settings,
  and cancellation/expiry/supersession after the network stage restore the OS.
- Explicit Armed survives pending capture and armed rollback; deliberate disarm
  removes its policy. Off/Armed/Connecting/Armed/Off uses fresh bounded Winsock
  connections to documentation-only `203.0.113.254:9`: access denial must follow
  Armed and the exact UI image permit must remove that denial in Connecting.
  A non-denied timeout/unreachable result is not Internet reachability.
- An abruptly terminated dual-stack child proves process-owned cleanup without
  destructors. Each child has a kill-on-close Job Object and a 60-second budget;
  every case must restore the complete observed network baseline and leave no
  product BFE objects within a separate ten-second cleanup bound.

The baseline comparison intentionally fails on unrelated route/DNS changes too:
freeze guest networking during the run. The retained difference counts and
snapshots distinguish owned policy residue from changed network fields; they do
not automatically attribute a physical-interface change to the test. The strict
ten-second live-state equality/zero-owned-object requirement remains unchanged.
Windows can retain an alias-only interface binding after the miniport is gone.
Only the exact synthetic test GUID and alias may be classified as retired, and
only with no route, DNS, address, or IPv4/IPv6 interface row plus an independent,
complete SetupAPI present-device scan proving that GUID absent. That record is
still retained in the snapshot. Present/unknown devices and foreign identities
are never excluded. Eleven synthetic controls exercise this exact discriminator
before OS mutation; `capture-native-tests.exe --disposable-vm --snapshot-selftest`
runs only those controls without elevation or any OS network access.

For root-cause collection only, `-DnsClearDiagnostic` selects one bounded child
instead of the nineteen acceptance cases. On its exact synthetic adapter it
records effective resolvers and `GetInterfaceDnsSettings` before/after production
rollback, at 1/5/10 seconds, then compares the return/state of null and non-null
empty DNS setter payloads with identical flags, retaining 1/5/10-second reads.
The getter follows its documented zero-input-flags contract; it is not claimed
as an IPv6-specific getter. The normal rollback assertions are unchanged, final
parent cleanup remains mandatory, and this mode's result is diagnostic-only,
never native acceptance. It needs a fresh output directory and the VM owner's
explicit approval; it performs test-adapter DNS writes but no DNS queries.

The native DNS comparator exposed a production rollback defect: with nameserver
and search-list flags set, null pointers returned `ERROR_INVALID_PARAMETER` (87)
and retained the injected resolver through ten seconds; non-null empty strings
returned success and removed it immediately. `NetworkConfig::ClearTunnelDns`
now prepares those empty values for both families and logs failed setters using
only the family and numeric error. `urnetworkd selftest` checks the same pure
production payload helper, including stale-field clearing and caller-owned string storage.
The unchanged native rollback assertion is the OS regression gate. Adapter
destruction remains the final cleanup layer; this defect does not establish a
provider-discovery or upstream resolver failure on an affected client.

A missing privilege/driver/BFE/API,
baseline access denial, timeout, or cleanup mismatch is a failure/prerequisite,
not a skipped pass. Never repair it by deleting unrelated routes or sweeping a
foreign policy owner. On abnormal termination or failed cleanup, stop using the
guest, retain the receipts and discard its disposable overlay via the existing
VM owner. Installing Wintun may leave its driver package in that overlay even
after all adapters/routes/filters are gone; this does not modify the base image.

Limits: proof is synthetic. The prepare marker and native ring are not the SDK
PacketPump. This does not test provider discovery, real provider packet delivery,
the service/RPC/UI bootstrap, real sleep/roam callbacks, split-driver enforcement,
MSI/SCM lifecycle, or upstream resolver reachability. Stage failures are injected
after successful OS effects, not forced failures inside every Windows API.
These need separate service/UI/native acceptance; authenticated provider cases
require explicitly authorized test-account/provider fixtures. The existing
`build/all/acceptance/run-windows.ps1` contacts MAIN and must not be used as a
credential-free substitute. The old documented P7 script is not present here.
