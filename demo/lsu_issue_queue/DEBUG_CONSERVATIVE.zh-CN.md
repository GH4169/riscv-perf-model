# conservative 模式断点调试手册

这份手册面向第一次边打断点边读 Olympia 代码的场景，目标是帮助你跑通：

    demo/lsu_issue_queue/run_demo.sh conservative

重点不是一次看完所有 SPARTA 细节，而是沿着一条访存指令的生命周期观察：它如何从
Dispatch 进入 LSU，经过 issue queue 的就绪判断和仲裁，再进入 AGEN/MMU/DL1/READ/WB
流水线，最后从队列中移除。推测模式的 abort/replay 放在文末作为第二阶段。

## 1. 先建立整体心智模型

### 1.1 从脚本到一次模拟

run_demo.sh 做三件事：

1. 用 CMake 生成 build-demo，构建类型是 Debug，并打开 compile_commands.json。
2. 构建目标 lsu_issue_queue_demo。
3. 直接执行二进制，并通过 -p 覆盖 allow_speculative_load_exec=false。

脚本实际执行的核心命令等价于（路径从 build-demo 工作目录解析）：

    ./demo/lsu_issue_queue/lsu_issue_queue_demo \\
      --input-file ../demo/lsu_issue_queue/trace.json \\
      -c ../demo/lsu_issue_queue/demo.yaml \\
      -p top.cpu.core0.lsu.params.allow_speculative_load_exec false \\
      -l top.cpu.core0.lsu info demo/lsu_issue_queue/output/conservative.lsu.log

脚本本身会运行到结束，不适合作为 GDB 的被调试进程。调试时使用同一个 Debug 二进制
和同一组参数直接启动 GDB 或 VS Code。

### 1.2 程序入口与调度循环

建议按这个顺序跳转源码：

    LsuIssueQueueDemo.cpp:main()
      -> CommandLineSimulator::parse()
      -> OlympiaSim::build/configure/bind tree
      -> sim.getRoot()->getChild("cpu.core0.lsu")
      -> scheduler.run(one clock period)
      -> LSUTester::capture(lsu)

LsuIssueQueueDemo.cpp 中的关键点：

- OlympiaSim sim(scheduler, 1, input_file, 7, false) 创建单核模拟，并把 ROB 退休上限设为 7 条。
- cls.populateSimulation(&sim) 建树、应用 demo.yaml 和命令行 -p 参数、绑定各单元端口。
- scheduler.run(cycle_ticks, true, false) 每次推进一个时钟周期。
- LSUTester::capture() 只读 LSU 内部容器；终端快照不是额外的调度逻辑。

树上的主要连接在 core/CPUTopology.cpp：

    dispatch.ports.out_lsu_write  -> lsu.ports.in_lsu_insts
    dispatch.ports.in_lsu_credits <- lsu.ports.out_lsu_credits

Dispatch 通过 LSU credit 控制是否还能接收访存指令，真正送入 LSU 的入口是
LSU::getInstsFromDispatch_()。

## 2. 七条指令为什么这样安排

trace.json 的 UID 按数组顺序生成：

| UID | 指令 | 你要观察的现象 |
| --- | --- | --- |
| 1 | div x10, x1, x2 | 长延迟产生 x10。 |
| 2 | lw x3, 0(x10) | 地址源依赖 UID 1，长期留在 IQ 中，状态为 N。 |
| 3 | lw x4, 0(x0) | 已就绪，可以越过 UID 2。 |
| 4 | lw x5, 0(x0) | 第二条越过 UID 2 的 ready load。 |
| 5 | sw x4, 0(x0) | 老 store；地址确认后，保守模式才允许后续 load。 |
| 6 | lw x6, 0(x0) | 与 UID 5 同址，最终展示 store-to-load forwarding。 |
| 7 | add x7, x6, x5 | 消费 load 结果，保证数据依赖影响退休。 |

UID 2/3/4 的虚拟地址分别为 0x1000/0x2000/0x3000，UID 5/6 都是 0x4000。
要分清两种阻塞：

- UID 2 是自己的地址操作数 x10 没准备好；两种模式都不能提前发射。
- UID 6 的地址操作数已准备好，但保守模式还要等待所有更老 store 的物理地址确认。

## 3. 第一次运行：先看基线

在仓库根目录运行：

    demo/lsu_issue_queue/run_demo.sh conservative

成功结束时会看到三项 PASS，并在 build-demo/demo/lsu_issue_queue/output/ 生成：

- conservative.observer.txt：逐周期快照，适合先看状态变化。
- conservative.lsu.log：LSU 函数级日志，适合和断点对应。

只看仲裁、转发和重发事件：

    rg "Arbitrated inst|Found forwarding store|Replay inst ready" \\
      build-demo/demo/lsu_issue_queue/output/conservative.lsu.log

一次实际运行的 issue history 是：

    #3 #4 #3 #4 #5 #6 #2 #2 #5

同一个 UID 出现多次是正常的：第一次是新发射，后面是 cache hit 后的 reload/pending
重发，不是重复生成了指令。

### 3.1 重要周期对照

观察器只在快照变化时打印，没有变化的周期不会显示。下面的周期来自当前 demo.yaml，
用于第一次下断点时定位，不是所有配置都固定不变的 ABI：

| 周期 | 现象 | 解释 |
| ---: | --- | --- |
| 8 | IQ 出现 #2:lw:N/LOW | UID 2 已 dispatch，但 x10 未 ready。 |
| 9 | ISSUE -> #3 | UID 3 越过阻塞的 UID 2。 |
| 10 | ISSUE -> #4 | UID 4 也越过 UID 2。 |
| 14 | ISSUE -> #4 | UID 4 的 cache hit reload。 |
| 15 | ISSUE -> #5 | 老 store 进入流水线。 |
| 17 | ISSUE -> #6 | UID 5 地址条件满足后才允许 UID 6。 |
| 20-22 | #6:lw(FWD) | UID 6 经过 DL1/READ/WB forwarding 路径。 |
| 32 | ISSUE -> #2 | UID 1 完成后，UID 2 的地址依赖解除。 |
| 40 | ISSUE -> #5 | store 的最终 pending/retry 路径。 |

LSU 日志的时间戳通常比观察器的 CYCLE 小 1；这是 scheduler 事件执行和 capture 时机
不同造成的。对照时优先按 UID 和日志文本匹配，不要机械比较两个数字。

## 4. 三轮断点练习

建议每轮只回答一个问题。第一次不要同时在 Fetch、Decode、Rename、ROB 和 LSU 上放
几十个断点。

### 练习 A：一条指令如何进入 LSU

断点：

    core/lsu/LSU.cpp:180  LSU::getInstsFromDispatch_()

每次停下先看：

    inst_ptr->getUniqueID()
    inst_ptr->getMnemonic()
    inst_ptr->getStatus()
    inst_ptr->isStoreInst()
    ldst_inst_queue_.size()
    store_buffer_.size()

然后单步进入：

    allocateInstToIssueQueue_()
      -> createLoadStoreInst_()
      -> ldst_inst_queue_.push_back()

    store 时再进入 allocateInstToStoreBuffer_()

UID 2/3/4 只进入 IQ，UID 5 额外进入 store buffer。队列按 dispatch 顺序保存，不代表
发射顺序。

### 练习 B：为什么 UID 3/4 能越过 UID 2

断点：

    core/lsu/LSU.cpp:195   LSU::handleOperandIssueCheck_()
    core/lsu/LSU.cpp:1238  LSU::arbitrateInstIssue_()
    core/lsu/LSU.cpp:307   LSU::issueInst_()

在 handleOperandIssueCheck_() 重点看三个分支：

1. instOperandReady_(inst_ptr)：检查地址源寄存器 scoreboard。
2. store 的 data operand 检查：store 需要地址和数据都 ready。
3. !allow_speculative_load_exec_ 时，load 还会调用 allOlderStoresIssued_(inst_ptr)。

UID 2 因 x10 不 ready 退出；UID 3/4 的 x0 已 ready，于是进入 appendToReadyQueue_()。
在 issueInst_() 中：

    ready_queue_.top() -> win_ptr
    ldst_pipeline_.append(win_ptr)
    win_ptr->setState(ISSUED)

注意 LoadStoreInstInfo::operator< 当前比较 UID，而不是 priority 数值，因此本版本 ready
queue 的 top 表现为 ready 项中 UID 较老者优先。priority 仍会在 reload/replay 时更新和
打印，但不参与这个比较器。

在 issueInst_() 停下后观察：

    win_ptr->getInstUniqueID()
    win_ptr->getMnemonic()
    win_ptr->getState()
    win_ptr->getPriority()
    ready_queue_.size()
    ldst_pipeline_.isAppended()

LSUTester::capture() 发生在 scheduler.run 返回之后，此时 ready queue 可能已经被
issueInst_() pop 空；所以快照里的 READY [] 不表示刚才没有 ready 指令。判断谁被选中，
以 ISSUE -> #uid 或 LSU log 的 Arbitrated inst 为准。

### 练习 C：沿着一条 load 走完流水线

断点顺序：

    core/lsu/LSU.cpp:344  handleAddressCalculation_()
    core/lsu/LSU.cpp:368  handleMMULookupReq_()
    core/lsu/LSU.cpp:403  getAckFromMMU_()
    core/lsu/LSU.cpp:455  handleCacheLookupReq_()
    core/lsu/LSU.cpp:628  handleCacheRead_()
    core/lsu/LSU.cpp:680  completeInst_()

demo.yaml 将 TLB 和 DCache 设置成 always hit；第一次学习可以把注意力放在状态转移，
不必先研究 miss。流水线槽位在 LSU 构造函数中计算：

    AGEN(stage 0) -> MMU -> DL1 -> READ -> WB

优先查看：

    ldst_pipeline_.isValid(stage)
    ldst_pipeline_[stage]
    MemoryAccessInfo::getPhyAddrStatus()
    MemoryAccessInfo::isCacheHit()
    MemoryAccessInfo::isDataReady()

UID 6 到 handleCacheLookupReq_() 时，tryStoreToLoadForwarding(inst_ptr) 命中 UID 5，
代码把 data ready 和 cache hit 置上，后续走 READ/WB，不必从 DCache 取得普通数据。

## 5. 启动调试器

### 5.1 VS Code（推荐）

仓库已有 .vscode/launch.json 和 .vscode/tasks.json。

1. 用 VS Code 打开仓库根目录。
2. 在“运行和调试”中选择 LSU Demo: conservative - issue arbitration。
3. 按 F5；preLaunchTask 会配置并编译 build-demo，并在 main() 停住。
4. 加入上面练习 A/B/C 的源码断点，再按 F5 继续。

配置中的参数与脚本一致：cwd 是 build-demo，allow_speculative_load_exec 是 false，
输入是 trace.json，配置是 demo.yaml。stopAtEntry=true 是有意保留的：先让可执行文件
和 Debug 符号加载，再添加 function breakpoint，断点状态更容易判断。

### 5.2 终端 GDB

先确保二进制存在：

    demo/lsu_issue_queue/run_demo.sh conservative

然后从仓库根目录启动：

    gdb --args build-demo/demo/lsu_issue_queue/lsu_issue_queue_demo \\
      --input-file demo/lsu_issue_queue/trace.json \\
      -c demo/lsu_issue_queue/demo.yaml \\
      -p top.cpu.core0.lsu.params.allow_speculative_load_exec false

在 GDB 中可使用：

    set breakpoint pending on
    b main
    b olympia::LSU::getInstsFromDispatch_
    b olympia::LSU::issueInst_
    run

如果函数名解析失败，先 run 到 main()，再执行断点命令；或者使用源码行断点。命中
issueInst_() 后，先看 win_ptr->getInstUniqueID()、win_ptr->getMnemonic()、
ready_queue_.size()，再用 next 单步。模板容器和 Sparta shared pointer 展开可能很吵，
先看 size、UID、状态和 priority。

## 6. 条件断点与日志对照

想只研究 UID 6 时，在 issueInst_() 的 win_ptr 已初始化之后（当前约为 314 行）设置：

    win_ptr->getInstUniqueID() == 6

在 getInstsFromDispatch_() 也可以使用：

    inst_ptr->getUniqueID() == 6

源码断点和日志事件的对应关系：

| 日志文本 | 主要源码 |
| --- | --- |
| New instruction added to the ldst queue | getInstsFromDispatch_() |
| Inst fully ready | handleOperandIssueCheck_() |
| Arbitrated inst | issueInst_() |
| Address Generation | handleAddressCalculation_() |
| Found forwarding store | handleCacheLookupReq_() |
| Completing inst | completeInst_() |

## 7. 第二阶段：再看 speculative

保守模式跑通后再执行：

    demo/lsu_issue_queue/run_demo.sh speculative

然后在 getAckFromMMU_() 和 abortYoungerLoads_() 下断点。重点观察：

    getAckFromMMU_()
      -> abortYoungerLoads_()
           -> setState(READY)
           -> appendToReadyQueue_()
           -> dropInstFromPipeline_()
           -> removeInstFromReplayQueue_()

UID 5 的物理地址确认发现同址的年轻 UID 6 后，UID 6 会从流水线撤掉、重新进入 ready
queue，再次发射并最终 forwarding。不要把这条路径和 conservative 下 UID 2 的“自己的
寄存器依赖解除”混为一谈。

## 8. 常见误区

### “IQ 里有 UID 2，为什么它没有马上发射？”

IQ 是生命周期容器，不是 ready queue。看 #2:lw:N/LOW 的 N，再回到
handleOperandIssueCheck_() 的 scoreboard 检查。

### “READY 一直是空的，是不是调度坏了？”

快照是在一次 scheduler.run 后抓取的，而 issue event 可能已经把 ready queue 的 top
pop 出去并 append 到 pipeline。看同一快照的 ISSUE -> #uid、PIPE ... NEXT=#uid 和 LSU log。

### “同一个 UID 为什么 issue 多次？”

cache hit、MMU pending 或 replay 会让一个 LoadStoreInstInfo 重新变成 ready，再次走
issueInst_()。这正是 issue queue 学习重点，不是 trace 被重复读取。

### “改了 YAML 后周期不一样了”

这是预期行为。队列大小、pipeline stage length、replay delay、cache/TLB 命中策略都会
改变事件时序；应比较状态转移和调用链，不要硬套上面的周期表。

## 9. 建议的阅读终点

完成三轮练习后，再按下面顺序扩展：

1. core/LoadStoreInstInfo.hpp：理解 state、priority、UID 比较器。
2. core/MemoryAccessInfo.hpp：理解虚拟地址、物理地址、cache/data ready。
3. core/lsu/LSU.hpp：把端口、队列、pipeline 和事件成员放回整体结构。
4. core/dispatch/Dispatch.cpp：理解 LSU credit 如何形成 backpressure。
5. core/CPUTopology.cpp：理解各单元端口如何绑定。
6. demo/lsu_issue_queue/LsuIssueQueueDemo.cpp：理解 observer 如何从内部状态重建可读输出。

读完后，你应该能从任意一个断点回答三件事：这条指令现在在哪个容器、为什么当前状态
是 R/I/N、下一步哪个事件会改变它。
