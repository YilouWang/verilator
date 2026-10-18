# 运行报告

每次运行在最上面加一节：本次做了什么、关键数字（附 verdict 文件路径）、失败了什么、下一步。不要用破折号。

## 第 2 次运行（2026-10-18 22:10 UTC 起）

### 做了什么
- 发现第 1 次运行的重排序等改进从未提交，已丢失；HEAD 只有 v1 门控。本次重新实现（v2）。
- 查明 smallboom 没有被门控的原因：立即断言生成纯的 AstCExpr（assertCtlGet），分析器把它当作未知副作用而整体关闭门控。现在纯 CExpr 只把语句标记为有副作用（总是执行，保持顺序）。
- v2 门控（提交 0d92231）：
  - 每个触发域内按数据流重排序：冲突边（读后写、写后读、写后写、有副作用语句链）上的拓扑排序，优先级取自从汇点出发的深度优先后序，有副作用的语句尽量靠后以便聚在一起。
  - 有副作用的 `if (c)`，若 c 是纯表达式，把 c 拆成单独的纯语句写入临时变量；这类条件语句组成条件组，条件变化时置脏，条件成立期间保持脏，所以断言和 printf 平时几乎不花时间。
  - 输出变化检测改在赋值处比较（`tmp = rhs; if (lhs != tmp) {...}`），不再做整块快照。
- 新增 rtlfast/tools/build.sh，用 oracle 的 harness 单独构建基准用于剖析。

### 数字
- 门限检查 PASS：rocket 300k 周期覆盖率 0.992，smallboom 150k 周期覆盖率 0.995（results/20261018-0d92231/check-*.json）。
- rocket 性能：3.281 倍，significance faster，中位数 56.2k 到 184.5k 周期每秒，离散 5%（results/20261018-0d92231/perf-rocket.json）。
- rocket 指令数（callgrind，2 万周期）：原版约 157k 每周期，v1 77k，v2 29k。
- smallboom 性能：0.839 倍，significance slower，22.0k 到 18.4k 周期每秒（results/20261018-0d92231/perf-smallboom.json）。正在分析。
- C++ 编译时间：rocket DUT 54 s 对原版 176 s，smallboom 98 s 对 291 s（-j4；注意 ccache 命中会干扰这些数字）。

### 失败与教训
- smallboom 门控后反而变慢，原因待查。
- 沙箱发生过一次虚拟机 fork，bootstrap 和心跳进程被杀，已手动重启心跳。
- 未提交的工作会丢失：以后长步骤前先提交或把补丁存到 journal。

### 下一步
- 对比 smallboom 原版和 v2 的 callgrind 剖析，修复变慢。
- 更便宜的脏标志检查（按 64 位字跳过），以便使用更小的分区。
