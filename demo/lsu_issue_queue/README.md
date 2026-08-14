# LSU Issue Queue Demo

For a step-by-step Chinese tutorial, including VS Code debugging, see
[`LEARNING_GUIDE.zh-CN.md`](LEARNING_GUIDE.zh-CN.md).

This demo runs the real Olympia core and prints the internal LSU issue queue state one cycle at a
time. It compares conservative load scheduling with speculative load execution using the same
seven-instruction JSON trace.

The columns show:

- `IQ`: dispatch order in `ldst_inst_queue_`
- `READY`: actual arbitration order in `ready_queue_`
- `PIPE`: AGEN, MMU, data-cache lookup/read, and completion stages
- `REPLAY`: speculative replay buffer
- `STORE`: store buffer

An entry such as `#2:lw:R/NEW` means instruction UID 2, mnemonic `lw`, ready state, and newly
dispatched priority.

## Trace design

| UID | Instruction | Purpose |
| --- | --- | --- |
| 1 | `div x10, x1, x2` | Produces a base register after a long execution latency. |
| 2 | `lw x3, 0(x10)` | Occupies the LSU queue while its own address operand is unavailable. |
| 3 | `lw x4, 0(x0)` | Ready load that can bypass UID 2. Its result feeds UID 5. |
| 4 | `lw x5, 0(x0)` | A second ready load that can bypass UID 2. |
| 5 | `sw x4, 0(x0)` | Older store whose address becomes known while UID 6 is in flight. |
| 6 | `lw x6, 0(x0)` | Same-address younger load used to show speculation, abort, replay, and forwarding. |
| 7 | `add x7, x6, x5` | Consumer that keeps the load result architecturally relevant. |

The JSON `vaddr` fields make the three independent loads use `0x1000`, `0x2000`, and `0x3000`;
UIDs 5 and 6 both use `0x4000`.

In conservative mode UID 6 waits until UID 5 has a known physical address. In speculative mode
UID 6 enters the pipeline earlier, then UID 5's MMU response detects the same address and aborts
UID 6. The load becomes ready again, reissues, and obtains its data through store-to-load
forwarding. UID 2 is different: its own address source is unavailable, so it cannot issue in either
mode until UID 1 finishes.

## Code map

- `core/lsu/LSU.cpp`: dispatch, readiness updates, arbitration, replay, and pipeline handlers
- `core/LoadStoreInstInfo.hpp`: queue-entry state and priority
- `core/lsu/LSU.hpp`: queue, replay buffer, store buffer, pipeline, and statistics
- `LsuIssueQueueDemo.cpp`: read-only cycle observer and executable self-checks
- `trace.json`: the seven-instruction workload
- `demo.yaml`: deliberately small queue and deterministic cache/MMU configuration

The observer uses the existing `LSUTester` friend declaration in `LSU`; it does not change the
production LSU implementation.

## Run

Run both modes from the repository root:

```bash
demo/lsu_issue_queue/run_demo.sh
```

Run one mode only:

```bash
demo/lsu_issue_queue/run_demo.sh conservative
demo/lsu_issue_queue/run_demo.sh speculative
```

The script expects Sparta `map_v2.2.3` under `../map/sparta/install`. Set `SPARTA_SEARCH_DIR` to
use another installed Sparta prefix.

Each mode ends with three `PASS` checks. Full cycle snapshots and LSU logs are written under
`build-demo/demo/lsu_issue_queue/output/`.
