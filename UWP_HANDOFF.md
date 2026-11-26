# UWP work handoff — 2026-11-12, updated 2026-11-13

## Current state (2026-11-13)

This section is newer than the rest of the file, which describes the state at the handoff of 2026-11-12.

* Branch `codex/uwp-runtime` and Wine `development` are at 9484da1a. The status table is in the parent checkout:
  `docs/uwp/README.md`; the list of Wine changes is `docs/STOCK_CHANGES.md` there.
* Done, measured on Windows 11 and passing on Wine x64 and x86 with the same binaries: package identity and
  activation (U1.0), view lifecycle (U1.1), presentation and input (U1.1b), package information (U1.2), package
  dependencies (U1.3), `ApplicationData` (U1.4), storage files, `FileIO`, `PathIO`, file streams, `DataReader`,
  `DataWriter` (U1.5).
* Where the code is: `server/uwp_context.c`, `dlls/twinapi.appcore`, `dlls/windows.ui`, `dlls/dxgi/factory.c`,
  `dlls/kernelbase/version.c`, `dlls/windows.storage.applicationdata`, `dlls/windows.storage` (`folder.c`, `fileio.c`,
  `filestream.c`, `vector.c`, `async.c`), `dlls/wintypes` (`datareader.c`, `storage.c`).
* Not done: suspend and resume (the Windows probe needs a person at the machine), MRT/PRI resources, installer
  signature check, server-side gate, `Windows.UI.Xaml` (plan: `docs/uwp/PLAN_XAML.md` in the parent, not started).
* Tests: the probes of `tests/uwp/u1_1` and `tests/uwp/u1_3` in the parent (`run-wine-lifecycle.sh`,
  `run-wine-deps.sh`), and the Wine tests of `windows.storage`, `wintypes`, `windows.storage.applicationdata`,
  `windows.ui`, `twinapi.appcore`. The two `np_basic` failures of the `rpcrt4` server test are older than this work.

## State and scope (2026-11-12)

The UWP feature is **not complete**. U1.0 requires a real AUMID activation of
our full-trust packaged fixture, consistent process/token identity and
package-local WinRT activation on the same binary bits as Windows. Direct
executable launch is only an unpackaged control. Lifecycle/CoreWindow, resource
and dependency closure, XAML and later work remain beyond that first gate.
The authoritative roadmap and security contract live in
`D:/DeepThought/Projects/WineExplorer/docs/uwp/README.md` and
`docs/uwp/IMPLEMENTATION_CONTRACT.md` in that parent checkout.

## Branches and changes since the previous activity

* This checkout is `C:/Users/Owner/.codex/worktrees/uwp-runtime/wine`, branch
  `codex/uwp-runtime`. Its code merge is `a524eaf34d2d16751fec757779b1aa8bb96f8462`:
  the earlier UWP base `c3f59e05` plus the development snapshot
  `c5823c9fe6370c0d62df280fda6e61bd97fe07e2`, which had advanced 24 commits
  in session-0, registry, token, process, services and other work. The merge
  applied without textual conflict. The parent/shared Wine checkout was not
  switched or changed by the merge. Development may advance further; recheck it
  before integration.
* UWP Wine commits before the merge: `c7be1378` repairs package full-name
  capacity and publisher encoding; `800dc315` adds token identity exports and
  validates unpackaged handle queries; `c91bcb35` adds initial StorageFolder
  and typed async support; `a441d03d` adds inert package-context storage and a
  standalone test; `bf8862fd` makes named-pipe RPC impersonation failures return
  non-success. The context object is not in the server Makefile, is not wired to
  any process/token or protocol, and authenticates nothing.
* The separate local tools repository is
  `D:/DeepThought/Projects/WineExplorer-UWP-Tools`, branch `codex/uwp-tools`.
  `6cb0fb0` is the bounded inspector baseline and `9fc0ad6` is an explicitly
  **WIP** development profile/stager/CLI snapshot. No remote or publication is
  configured. Its stager must not be treated as installation or catalog
  authority: cleanup and ancestor/name replacement races remain under review.
  Trust and signatures remain unverified; dependencies remain unresolved.
* Parent WineExplorer has advanced independently in system-tier/elevation work.
  Its shared checkout and its other agents' changes must not be reset, staged,
  or modified as part of this handoff. The parent gitlink is not updated to
  this UWP branch. The parent `docs/uwp` planning/review files have uncommitted
  UWP notes; `tests/uwp/context-observations/`, `tests/uwp/storage/`, and the
  partial `tests/uwp/u1_0_prepare/` are also uncommitted. The latter preparation
  script was left by a cancelled Flash job and has not been reviewed or run.
  Preserve them and the unrelated untracked files in that checkout.
* Parent commit `90165467` records the reviewed package-name native observation
  fixture and its review. The original Raven reuse note was reviewed in
  `docs/uwp/REVIEW_RAVEN_REUSE.md`; no Raven code was imported.

## Evidence and limits

* A clean pre-merge combined WoW64 build from the isolated UWP source completed
  with `--enable-archs=i386,x86_64`. Fresh-prefix tests on each architecture
  passed 599 StorageFolder checks with no skips and 541 kernel32 version checks
  with two skips; matching wineserver cleanup was `kill=0 wait=0`. The later
  metadata sentinel assertion was separately rebuilt and passed in the
  incremental candidate. These results **do not test the merged development
  snapshot** or packaged activation.
* On native Windows x64 and x86, the StorageFolder statics and activation
  factory `GetRuntimeClassName` returned `E_ILLEGAL_METHOD_CALL` and cleared a
  prefilled output pointer. The draft now follows that observed case. Native
  statics `GetIids` returned three IDs; the draft exposes only its implemented
  interfaces and therefore does not have full metadata parity. Native behavior
  for several path and NULL-pointer cases is unmeasured; unmeasured tests are
  restricted to Wine. See the uncommitted parent
  `docs/uwp/REVIEW_STORAGE_FOLDER.md` and owned scratch probes.
* RPC correction built for x64/x86. The broader server test suite reported two
  client-PID failures per ABI; the unchanged pre-patch baseline reproduced the
  same failures. The specific impersonation failure branch has **not** been
  fault-injected. `CoImpersonateClient`/COM call-context linkage and actual
  caller token still require runtime proof; do not fall back to the server's
  ambient token. Preserve the existing elevation and E1 authorization checks.
* The inert context module compiled against server headers with `-Wall -Werror`.
  Its standalone 21 checks cover deep copies/accessors, lifetime, empty and
  embedded-NUL fields, invalid size and injected allocation failure. It does not
  establish context provenance, process/token attachment or persistence.
* The tools snapshot passed 116 tests in the owned guest. Passing tests do not
  resolve the stager's same-user/ancestor replacement and cleanup races.
* Native x64 packaged activation has historical fixture evidence. Native x86
  packaged activation, the current same-binary package comparison, and all Wine
  packaged cases remain unverified. Do not install or trust a certificate using
  the older scripts until their ownership and cleanup are reviewed.

## Owned lab and merged-build check

The dedicated VM is controlled by the parent repository's
`scripts/lab/uwp/owned-lab.py`; it is separate from elevation VMs. Its workspace
is `/home/debian/uwp-u1-20261111` and owner marker is
`codex-uwp-u1-20261111`. Use the controller's `status`/`ssh`/`push` actions;
never reset or clean shared VMs. The guest build volume is mounted at
`/home/debian/uwp-u1-20261111/build`.

A clean merged-source build for code commit `a524eaf3` was started at
`build/candidates/merged-a524eaf3`. Its source archive SHA-256 is
`9b89e7e7806ed7c18d293c853a14870efd9de57d9318396910b75c3b5b046aec`;
check `phase`, `exit`, `archive-check.log`, `configure.log`, and `build.log`
before claiming success. It was **in progress** when this handoff was written.
The pre-merge clean candidate and logs remain in `build/candidates/r4-clean-1`;
the RPC patch/baseline comparisons are in `r4-1/rpc-tests` and
`r0a-1/rpc-tests`, respectively. Test/cleanup logs are prefix-specific.

## Next actions

1. Finish and review the merged clean build, then run both-ABI kernel32 version,
   StorageFolder and RPC regressions in fresh owned prefixes; distinguish the
   known baseline RPC client-PID failures from new failures. Exercise the RPC
   impersonation failure branch with a meaningful controlled fault test.
2. Resolve and enforce the staging trust/ownership and atomic publication
   contract before connecting staged files to any authoritative catalog.
3. Design the server-owned package catalog/context and one-shot AUMID activation
   with provenance bound to the actual caller and verified image. Account for
   process creation, token duplication and primary-token replacement; no env,
   PEB, caller-provided identity strings, ambient server token or privilege
   relaxation may substitute for the binding. The compatibility gate is exact
   DWORD 1 and defaults off. Reconcile all changes with the newer system-tier
   and elevation work.
4. Implement and verify the package-local WinRT resolver without changing the
   ordinary unpackaged lookup. Distinguish no context from a declared class
   whose package DLL fails. Establish observed Windows fallback behavior.
5. Complete safe fixture preparation and both native package oracles, then run
   the same unchanged binaries and negative cases in a fresh Wine prefix. U1.0
   is not done until those checks pass; U1.1 and later gates remain separate.

The user prefers implementation progress with proportionate validation. Qwen
and Antigravity can implement; Codex plans/reviews and handles difficult setup,
and may make straightforward fixes directly. The last selected Antigravity
model was Gemini 3.8 Flash. A minimal readiness job succeeded, while several
larger jobs were cancelled after prolonged silence; no quota exhaustion was
reported. Check durable job status rather than assuming a timeout or quota.
