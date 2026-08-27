# mprpc 压力测试报告

测试日期：2026-08-17

## 目标

验证 mprpc 在短连接模式下的基线性能，并评估长连接复用对端到端 RPC 吞吐与尾延迟的影响。

## 测试范围与环境

- 单机回环：客户端与 provider 均运行在 `127.0.0.1`。
- 单 provider 实例：端口 `8001`。
- 服务发现：本机 ZooKeeper，端口 `2181`。
- 请求：`OrderServiceRPC.check`，请求订单号为 `001`。
- 编解码：Protobuf。
- 业务逻辑：内存中的订单查询，返回创建时间和价格；不包含数据库、磁盘或下游网络调用。
- 压测程序：`bin/rpc_benchmark`。
- 采样方式：各档预热 2 秒；统计成功数、失败数、QPS、平均延迟、P50、P95、P99 和最大延迟。

> 本报告是单机轻量 RPC 的端到端结果，不能直接与跨机器、TLS、多路复用或包含复杂业务逻辑的公开基准横向比较。

## 压测前的传输层改进

1. 请求和响应均采用长度帧，响应格式为 `response_size(4 bytes) + protobuf response`。
2. 客户端以循环方式完成 `send` 与 `recv`，避免 TCP 部分读写导致报文不完整。
3. 服务端在 Muduo `Buffer` 中等待完整请求帧，避免半包和粘包被提前解析。
4. 逐请求调试输出默认关闭，避免控制台 I/O 干扰吞吐。

## 模式说明

### 短连接

默认模式，保持原先的调用语义：

```text
socket -> connect -> request -> response -> close
```

配置：`bin/test2.conf` 与 `test/benchmark/benchmark.conf`。

### 长连接

长连接通过配置显式开启，短连接代码与默认行为保留：

```text
首次调用：socket -> connect
后续调用：复用同一个 MprpcChannel 到同一 provider 的 TCP 连接
```

当前实现中一条连接同一时刻只处理一个请求，不包含 request ID、多路复用或异步 in-flight 请求。

配置：

- provider：`bin/test_long.conf`，设置 `rpcserverlongconnection=true`
- client：`test/benchmark/benchmark_long.conf`，设置 `rpcclientlongconnection=true`

## 执行方法

确保 ZooKeeper 已运行后，启动 provider：

```bash
# 短连接
./bin/provider -i bin/test2.conf

# 长连接
./bin/provider -i bin/test_long.conf
```

另一个终端执行压测：

```bash
# 短连接
./bin/rpc_benchmark -i test/benchmark/benchmark.conf \
  --threads 16 --warmup 2 --duration 5

# 长连接
./bin/rpc_benchmark -i test/benchmark/benchmark_long.conf \
  --threads 16 --warmup 2 --duration 5
```

## 短连接结果

| 并发线程 | 采样时长 | 成功/失败 | QPS | 平均延迟 | P50 | P95 | P99 | 最大延迟 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 5 s | 14,309 / 0 | 2,861 | 0.346 ms | 0.250 ms | 0.759 ms | 1.736 ms | 36.075 ms |
| 4 | 10 s | 114,919 / 0 | 11,490 | 0.345 ms | 0.312 ms | 0.618 ms | 0.973 ms | 5.309 ms |
| 16 | 5 s | 94,046 / 0 | 18,800 | 0.848 ms | 0.803 ms | 1.494 ms | 2.128 ms | 16.259 ms |
| 64 | 5 s | 93,669 / 0 | 18,721 | 3.414 ms | 3.304 ms | 5.601 ms | 7.062 ms | 19.595 ms |

短连接在约 16 线程后吞吐趋于饱和，继续提高并发主要增加排队和尾延迟。每次请求的 TCP 建连与断连是主要额外开销。

## 长连接结果

| 并发线程 | 采样时长 | 成功/失败 | QPS | 平均延迟 | P50 | P95 | P99 | 最大延迟 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 5 s | 41,519 / 0 | 8,303 | 0.118 ms | 0.102 ms | 0.166 ms | 0.498 ms | 3.000 ms |
| 4 | 5 s | 167,759 / 0 | 33,538 | 0.117 ms | 0.111 ms | 0.168 ms | 0.439 ms | 3.743 ms |
| 16 | 5 s | 399,883 / 0 | 79,892 | 0.197 ms | 0.160 ms | 0.444 ms | 0.807 ms | 6.395 ms |
| 64 | 5 s | 485,586 / 0 | 96,948 | 0.656 ms | 0.551 ms | 1.435 ms | 2.601 ms | 13.185 ms |

## 短连接与长连接对比

| 并发线程 | 短连接 QPS | 长连接 QPS | 吞吐提升 | 短连接 P99 | 长连接 P99 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2,861 | 8,303 | 2.90x | 1.736 ms | 0.498 ms |
| 4 | 11,490 | 33,538 | 2.92x | 0.973 ms | 0.439 ms |
| 16 | 18,800 | 79,892 | 4.25x | 2.128 ms | 0.807 ms |
| 64 | 18,721 | 96,948 | 5.18x | 7.062 ms | 2.601 ms |

结论：连接复用显著减少了 TCP 建连和断连成本。64 并发下，吞吐从约 18.7k QPS 提升至约 96.9k QPS，同时 P99 从 7.062 ms 降至 2.601 ms。

## 当前限制与后续建议

1. 基准持续时间较短；建议补充 16 并发、预热 10 秒、持续 10 至 60 分钟的稳定性测试。
2. 应同时采集 provider 的 CPU、RSS、文件描述符数和 TCP `TIME_WAIT` 状态。
3. 应增加多 provider 实例，测试 ZooKeeper 服务发现和负载均衡的横向扩展能力。
4. 可进一步实现连接池、请求 ID 和单连接多路复用，并与当前“每连接单请求”的长连接实现对比。
5. 可测试服务端下线、重启、超时及熔断后的恢复行为。
