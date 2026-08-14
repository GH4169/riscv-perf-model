# LSU 访存指令发射队列 Demo 学习指南

这份文档用于带着代码学习 Olympia 的访存指令发射队列。Demo 使用的不是重新编写的简化 LSU，而是项目中的真实 `olympia::LSU`，只增加了一个只读观察器来逐周期打印内部状态。

## 1. 学习目标

完成本指南后，应当能够回答下面几个问题：

1. load/store 指令如何进入 `ldst_inst_queue_`。
2. 一条访存指令什么时候是 `READY`、`ISSUED` 或 `NOT_READY`。
3. `ready_queue_` 如何选出下一条待发射指令。
4. 为什么年轻的 ready load 可以越过一条被依赖阻塞的老 load。
5. 保守 load 调度和推测 load 调度有什么区别。
6. 年轻 load 为什么会被终止、重新变成 ready，并再次发射。
7. store-to-load forwarding 在流水线的哪个阶段发生。

## 2. Demo 文件

| 文件 | 作用 |
| --- | --- |
| `demo/lsu_issue_queue/LsuIssueQueueDemo.cpp` | 启动真实模型，逐周期读取 LSU 内部状态，并执行结果自检 |
| `demo/lsu_issue_queue/trace.json` | 7 条教学指令组成的输入 trace |
| `demo/lsu_issue_queue/demo.yaml` | 缩小队列并固定 MMU、Cache 行为，减少无关随机性 |
| `demo/lsu_issue_queue/run_demo.sh` | 配置、编译并运行保守和推测模式 |
| `.vscode/tasks.json` | VS Code 配置和编译任务 |
| `.vscode/launch.json` | 三套 GDB 调试入口 |

观察器使用 `LSU.hpp` 中已有的 `friend class LSUTester` 访问内部成员，没有修改生产 LSU 的调度行为。

## 3. 快速运行

在仓库根目录执行：

```bash
cd /home/zhourongyi/project/riscv-perf-model
demo/lsu_issue_queue/run_demo.sh
```

该命令会依次运行：

1. conservative：保守 load 调度。
2. speculative：推测 load 调度。

只运行一种模式：

```bash
demo/lsu_issue_queue/run_demo.sh conservative
demo/lsu_issue_queue/run_demo.sh speculative
```

脚本会自动执行 CMake 配置和 Debug 编译。默认使用：

```text
Sparta: ../map/sparta/install
Build:  build-demo
```

使用其他 Sparta 安装目录时：

```bash
SPARTA_SEARCH_DIR=/path/to/sparta/install \
    demo/lsu_issue_queue/run_demo.sh conservative
```

运行成功后，结尾应出现三项 `PASS`：

```text
[PASS] ready loads #3/#4 issued before blocked older load #2
[PASS] ...
[PASS] same-address load #6 used store-to-load forwarding
```

完整输出保存在：

```text
build-demo/demo/lsu_issue_queue/output/conservative.observer.txt
build-demo/demo/lsu_issue_queue/output/conservative.lsu.log
build-demo/demo/lsu_issue_queue/output/speculative.observer.txt
build-demo/demo/lsu_issue_queue/output/speculative.lsu.log
```

其中 `observer.txt` 适合先看整体周期变化，`lsu.log` 适合对应到具体 C++ 函数。

## 4. 七条指令的设计

| UID | 指令 | 在 Demo 中的作用 |
| --- | --- | --- |
| 1 | `div x10, x1, x2` | 长延迟地产生 `x10` |
| 2 | `lw x3, 0(x10)` | 因地址源寄存器未就绪而长期占据 LSU IQ |
| 3 | `lw x4, 0(x0)` | 已就绪的年轻 load，可以越过 UID 2；结果供 UID 5 使用 |
| 4 | `lw x5, 0(x0)` | 第二条可以越过 UID 2 的 ready load |
| 5 | `sw x4, 0(x0)` | 老 store；其地址确认会检查已推测执行的年轻 load |
| 6 | `lw x6, 0(x0)` | 与 UID 5 同址，用来展示推测、终止、重发和转发 |
| 7 | `add x7, x6, x5` | 消费 load 结果，使数据依赖在体系结构上有效 |

trace 中 UID 2、3、4 的虚拟地址分别是 `0x1000`、`0x2000`、`0x3000`，UID 5 和 UID 6 都使用 `0x4000`。

这里要区分两种阻塞：

- UID 2 自己的地址操作数 `x10` 没准备好，因此保守和推测模式都不能提前发射它。
- UID 6 自己的操作数已经准备好，但前面存在地址尚未确认的 store。是否允许它提前发射，由 `allow_speculative_load_exec` 决定。

## 5. 如何读逐周期输出

典型输出如下：

```text
CYCLE 17  SPECULATIVE ABORT -> #6 READY AGAIN
  IQ      [#2:lw:N/LOW, #5:sw:I/LOW, #6:lw:R/LOW]
  READY   [#6:lw:R/LOW]
  PIPE    AGEN=- | MMU=#5:sw | DL1=- | READ=- | WB=-
  REPLAY  [#5:sw]
  STORE   [#5:sw]
```

各栏含义：

| 栏位 | 对应实现 | 含义 |
| --- | --- | --- |
| `IQ` | `ldst_inst_queue_` | 所有仍由 LSU 管理的访存指令，顺序是 dispatch 顺序 |
| `READY` | `ready_queue_` | 当前可以参与发射仲裁的指令 |
| `PIPE` | `ldst_pipeline_` | AGEN、MMU、DL1 lookup、cache read 和完成阶段 |
| `REPLAY` | `replay_buffer_` | 推测模式下仍可能被终止或需要重试的指令 |
| `STORE` | `store_buffer_` | 尚未完成体系结构提交处理的 store |

指令格式是：

```text
#UID:mnemonic:state/priority
```

状态：

- `R`：`READY`，可以进入发射仲裁。
- `I`：`ISSUED`，已经进入访存流水线。
- `N`：`NOT_READY`，依赖、store 地址或 replay 条件尚未满足。

常见 priority：

- `NEW`：新 dispatch 后的首次发射。
- `C-RELOAD`：Cache 响应后需要重新访问流水线。
- `C-PEND`：等待已有 Cache 操作完成。
- `M-RELOAD`、`M-PEND`：对应 MMU 路径。
- `LOW`：当前没有更高的重发优先级标记。

需要注意：在当前项目版本中，`ready_queue_` 的 `operator<` 比较的是 UID，而不是 `IssuePriority`。因此 `arbitrateInstIssue_()` 调用 `ready_queue_.top()` 时，实际表现为 ready 指令中的老指令优先。priority 仍会被维护并显示，但目前没有被该比较器用于仲裁。对应代码在 `core/LoadStoreInstInfo.hpp` 和 `LSU::arbitrateInstIssue_()`。

## 6. 推荐的学习顺序

### 第一步：只看保守模式

```bash
demo/lsu_issue_queue/run_demo.sh conservative
```

重点观察：

1. 第 8 周期，UID 2 在 `IQ` 中为 `N`。
2. 第 9、10 周期，UID 3 和 UID 4 先后发射，证明 IQ 不是严格按程序顺序发射。
3. UID 6 必须等 UID 5 的地址条件满足后才能发射。
4. UID 6 进入 DL1 阶段后出现 `(FWD)`。
5. UID 1 的长延迟执行结束后，UID 2 才变为 ready 并发射。

### 第二步：看推测模式

```bash
demo/lsu_issue_queue/run_demo.sh speculative
```

重点观察：

1. UID 6 比保守模式更早进入流水线。
2. UID 5 的 MMU 地址确认触发 `abortYoungerLoads_()`。
3. UID 6 从 `ISSUED` 回到 `READY`，输出显示 `SPECULATIVE ABORT`。
4. UID 6 再次发射，最终从 UID 5 转发数据。

### 第三步：比较关键日志

```bash
rg "Arbitrated inst|Aborted younger load|Found forwarding store" \
    build-demo/demo/lsu_issue_queue/output/speculative.lsu.log
```

再与保守模式比较：

```bash
rg "Arbitrated inst|Aborted younger load|Found forwarding store" \
    build-demo/demo/lsu_issue_queue/output/conservative.lsu.log
```

## 7. 对照源码阅读

建议按下面的调用路径阅读，而不是从 `LSU.cpp` 第一行顺序往下读。

### 7.1 Dispatch 和就绪判断

```text
LSU::getInstsFromDispatch_()
  -> allocateInstToIssueQueue_()
  -> allocateInstToStoreBuffer_()    // 仅 store
  -> handleOperandIssueCheck_()
       -> instOperandReady_()
       -> allOlderStoresIssued_()    // 保守 load 模式
       -> appendToReadyQueue_()
```

在 `handleOperandIssueCheck_()` 重点看：

- 地址源寄存器如何通过 scoreboard 检查。
- store 如何额外检查 data operand。
- `allow_speculative_load_exec_ == false` 时，load 为什么还要调用 `allOlderStoresIssued_()`。

### 7.2 发射仲裁

```text
LSU::issueInst_()
  -> arbitrateInstIssue_()
  -> ldst_pipeline_.append()
  -> appendToReplayQueue_()          // 推测模式
  -> state = ISSUED
```

关键对象：

- `ldst_inst_queue_`：生命周期和容量管理。
- `ready_queue_`：真正参与仲裁的 priority queue。
- `LoadStoreInstInfo`：保存状态、priority、指令和访存信息。

### 7.3 访存流水线

```text
handleAddressCalculation_()
  -> handleMMULookupReq_()
  -> getAckFromMMU_()
  -> handleCacheLookupReq_()
  -> handleCacheRead_()
  -> completeInst_()
```

`demo.yaml` 将 MMU 和 DCache 配置成 always hit，是为了让学习重点集中在发射和依赖，而不是 Cache miss 的复杂时序。

### 7.4 推测终止和重发

```text
getAckFromMMU_()
  -> abortYoungerLoads_()
       -> appendToReadyQueue_()
       -> dropInstFromPipeline_()
       -> removeInstFromReplayQueue_()
```

UID 5 和 UID 6 的地址相同，因此 UID 5 的地址确认会找到 UID 6，并把 UID 6 从流水线中移除。它的状态重新变成 `READY`，之后可以再次参与仲裁。

### 7.5 Store-to-load forwarding

在 `handleCacheLookupReq_()` 和 `tryStoreToLoadForwarding()` 附近观察：

- load 如何搜索 store buffer。
- 地址相同且 store data ready 时，load 如何跳过普通 Cache 数据来源。
- Demo 输出为什么会在流水线指令后标出 `(FWD)`。

## 8. 使用 VS Code 调试

### 8.1 准备 VS Code

用 VS Code 打开仓库根目录：

```bash
cd /home/zhourongyi/project/riscv-perf-model
code .
```

安装扩展：

```text
Microsoft C/C++
扩展 ID: ms-vscode.cpptools
```

仓库中的 `.vscode/extensions.json` 会自动显示推荐安装提示。本机已经安装 GDB。VS Code 通过 `.vscode/gdb-no-debuginfod.sh` 调用 `/usr/bin/gdb`，避免启动时等待系统库调试符号下载。

### 8.2 构建任务

按 `Ctrl+Shift+P`，选择 `Tasks: Run Task`，可以运行：

- `LSU Demo: configure`
- `LSU Demo: build`
- `LSU Demo: prepare debug`
- `LSU Demo: run all`

正常情况下不需要手动先执行任务。下面的调试配置都有 `preLaunchTask`，按 F5 时会自动配置和编译 Debug 版本。

### 8.3 两套调试配置

打开 VS Code 左侧“运行和调试”，选择：

1. `LSU Demo: conservative - issue arbitration`
   使用保守模式启动。适合在 `LSU::issueInst_()` 观察 `ready_queue_` 选出了谁。

2. `LSU Demo: speculative - load abort`
   使用推测模式启动。适合在 `LSU::abortYoungerLoads_()` 跟踪 UID 6 被终止并重新放回 ready queue 的过程。

两个配置都会先安全地停在 `main()`。这是为了让可执行文件和符号先由 VS Code 正式加载，避免由 GDB 私自创建的断点被 VS Code 误报为异常。

第一次使用某个配置时：

1. 选择配置并按 `F5`，程序停在 `main()`。
2. 在左侧 `BREAKPOINTS` 区域单击 `+`，选择 `Add Function Breakpoint`。
3. 保守模式输入 `olympia::LSU::issueInst_`；推测模式输入 `olympia::LSU::abortYoungerLoads_`。
4. 再按 `F5`，程序会停在目标 LSU 函数。

该 function breakpoint 由 VS Code 管理并会保存在工作区中，后续调试不需要重复添加。每次启动仍会先停在 `main()`，再按一次 `F5` 即可继续到 LSU 断点。

### 8.4 推荐的单步操作

- `F10`：Step Over，执行当前行但不进入被调用函数。
- `F11`：Step Into，进入当前调用的函数。
- `Shift+F11`：Step Out，运行到当前函数返回。
- `F5`：Continue，运行到下一个断点。
- 在编辑器行号左侧单击：添加或删除源码断点。

建议第一次调试发射流程时：

1. 选择 conservative 配置并按 F5。
2. 在 `main()` 停下后添加 `olympia::LSU::issueInst_` function breakpoint。
3. 再按 F5，在 `issueInst_()` 停下后观察 `ready_queue_`。
4. 用 F10 执行 `arbitrateInstIssue_()`。
5. 观察局部变量 `win_ptr` 的 UID、状态和 priority。
6. 用 F11 进入 `arbitrateInstIssue_()`，确认它读取 `ready_queue_.top()`。
7. 连续按 F5，对比 UID 3、4、5、6、2 的发射顺序。

### 8.5 推荐观察的变量

在 VS Code 的 `WATCH` 面板中添加：

```text
allow_speculative_load_exec_
ldst_inst_queue_.size()
ready_queue_.size()
replay_buffer_.size()
store_buffer_.size()
ldst_pipeline_.isAppended()
```

在 `issueInst_()` 执行过 `arbitrateInstIssue_()` 后，再观察：

```text
win_ptr
win_ptr->getInstUniqueID()
win_ptr->getMnemonic()
win_ptr->getState()
win_ptr->getPriority()
```

在 `abortYoungerLoads_()` 中观察：

```text
inst_ptr->getUniqueID()
inst_ptr->getTargetVAddr()
min_inst_age
replay_buffer_.size()
replay_inst->getInstUniqueID()
replay_inst->getState()
```

部分表达式只有在对应局部变量进入作用域后才有效。显示 `not available` 时，先单步执行到变量定义之后。

### 8.6 条件断点建议

如果不想每条访存指令都停住，可以在 `issueInst_()` 中 `win_ptr` 已经赋值之后的源码行添加条件断点：

```text
win_ptr->getInstUniqueID() == 6
```

这样只观察 UID 6 的首次发射和再次发射。

在 `abortYoungerLoads_()` 遍历 replay buffer 的循环内部，也可以使用：

```text
replay_inst->getInstUniqueID() == 6
```

## 9. 修改 Demo 做实验

### 修改推测开关

运行脚本已经通过参数覆盖：

```text
top.cpu.core0.lsu.params.allow_speculative_load_exec
```

也可以在 `.vscode/launch.json` 中把最后的 `true` 或 `false` 改掉。

### 修改队列容量

编辑 `demo.yaml`：

```yaml
lsu.params:
  ldst_inst_queue_size: 4
  replay_buffer_size: 8
  replay_issue_delay: 3
```

建议实验：

1. 把 `ldst_inst_queue_size` 改成 2，观察 dispatch backpressure。
2. 修改 `replay_issue_delay`，观察 store pending 重试节奏。
3. 关闭 `allow_data_forwarding`，观察 UID 6 的完成路径变化。

### 修改 trace

可以编辑 `trace.json` 增加 load/store 组合。当前 Demo 的 ROB 退休上限固定为 7 条指令；如果改变指令数量，需要同步修改 `LsuIssueQueueDemo.cpp` 中 `OlympiaSim` 构造函数传入的退休上限，并调整针对 UID 2、3、4、6 的自检。

## 10. 常见问题

### F5 提示找不到可执行文件

先运行：

```bash
demo/lsu_issue_queue/run_demo.sh conservative
```

确认下面文件存在：

```text
build-demo/demo/lsu_issue_queue/lsu_issue_queue_demo
```

### CMake 找不到 Sparta

默认查找：

```text
${workspaceFolder}/../map/sparta/install
```

如果安装位置不同，修改 `.vscode/tasks.json` 中的 `SPARTA_SEARCH_DIR` 参数，或者在终端运行脚本时设置同名环境变量。

### 断点显示为空心圆

依次检查：

1. 使用的是 `build-demo` 中的 Debug 可执行文件。
2. `preLaunchTask` 已成功完成。
3. 打开的源码属于当前仓库，而不是另一份 checkout。
4. GDB 路径是 `/usr/bin/gdb`。

function breakpoint 在启动早期短暂显示为 pending 是正常现象；目标文件加载后，它会自动解析到对应的 LSU 源码位置。

### 出现 `Hit breakpoint 1 at ...` 异常提示

旧版 `launch.json` 在 `setupCommands` 中直接创建 GDB 断点。GDB 能命中该断点，但 VS Code 不认识它的编号，因此会把正常停顿显示为异常。当前配置已经移除了这种断点。执行 `Developer: Reload Window` 重新加载工作区配置，然后按照 8.3 节使用 VS Code function breakpoint。

### 变量太复杂，不容易展开

SPARTA 容器中包含迭代器和智能指针，全部展开会产生大量字段。先看 `size()`、UID、state 和 priority，再按需要展开 `Inst` 或 `MemoryAccessInfo`，学习效率更高。
