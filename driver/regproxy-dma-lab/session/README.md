# MT7927 实验中的 KMDF 公共缓冲区

这份源码供 [NekoDrop #57](https://github.com/zheznanohana/nekodrop/issues/57) 对照 MT7927 和 MT7925 的 DMA 管理实现。编译入口是 [ring_session_owner.c](ring_session_owner.c)。实测时，UMDF 上层驱动把文件的创建、清理和关闭转交给同一设备栈中的 KMDF 下层驱动。这里共享的是下层源码。

| 对照点 | 7927 的实现 |
| --- | --- |
| DMA Enabler | [SessionPrepare](session_owner.c) 用设备对象创建 `WdfDmaProfileScatterGatherDuplex` enabler，指定 WDM DMA v3。六队列实验中，单次传输长度上限为 32 KiB。 |
| 地址和对齐 | [AllocatePrivateRings](ring_session_buffers.h) 用同一个 enabler 分配每块内存，分别取得 CPU 虚拟地址和设备逻辑地址。[mt7927_image_valid](patch_ring_image.h) 检查长度、16 字节对齐、整段逻辑地址能否放进 32 位字段，以及各段是否重叠。MT7925 的字段宽度和对齐要求应由它自己的描述符确认。 |
| 关闭与回收 | [SessionCleanup 和 SessionRelease](session_owner.c) 在同一把锁下处理最后一个文件句柄关闭及设备释放。[DeletePrivateRings](ring_session_buffers.h) 按分配的逆序释放，分配中途失败也走这条路径。这里的缓冲区始终由主机私有，队列地址尚未写给网卡。 |

**MT7927 实测。** 六个队列共分配 167 块缓冲区，同一次会话的持有字节数从 0 升至 5,316,848，关闭后回到 0。另一次 4 KiB 实验把句柄复制到另一进程，强制结束原进程后缓冲区仍保留，最后一个句柄关闭才回收。网卡取得地址后如何确认在途 DMA 全部结束，仍待实测。

这个版本用 x64 WDK 10.0.26100.0、KMDF 1.33 重新编译和链接，未签名下层驱动的 SHA-256 与 0.3 实机构建一致。环布局沿用的 mt76 源码版本和许可见[来源说明](../../../mtradio/LICENSE.mt7927-mt76)。
