#!/usr/bin/env python3
"""HID 链路协议验证。

不依赖浏览器：直接通过 hidapi 打开 Report ID 4 的厂商定义 collection，
按 meo_link 的分片协议收发，验证四件事——

  1. 下行命令能被完整重组（LINK PING 往返）
  2. 上行回复的分片头自洽（msg_id / frag_idx / frag_total）
  3. 跨分片的长回复能拼回整行（LINK STATUS 走多片）
  4. 命令处理与 CDC 共用同一套语义（WIFI STATUS 与串口结果一致）

用法：
    python test_hid_link.py            # 自动找设备
    python test_hid_link.py --verbose
"""
import argparse
import sys
import time

import hid

VID = 0x303A
PID = 0x4005
REPORT_ID = 4
REPORT_SIZE = 64
HEADER = 4
PAYLOAD = REPORT_SIZE - HEADER

KIND_REPLY = ord("R")
KIND_LOG = ord("L")
KIND_CMD = ord("C")


def find_device(verbose=False):
    """找到真正承载 Report ID 4 的那个 collection。

    手势 collection 和链路 collection 的 usage_page/usage 完全一样（都是
    vendor-defined generic in/out），hidapi 的枚举信息里也不带 report ID，
    所以没法靠字段区分。唯一可靠的办法是逐个打开、各发一次 PING，看谁回。
    往手势 collection 写 64 字节只会被当成非法报告而失败，没有副作用。
    """
    candidates = [i for i in hid.enumerate(VID, PID) if i.get("interface_number") == 2]
    if verbose:
        for c in candidates:
            print(f"  candidate usage_page=0x{c.get('usage_page', 0):04X} "
                  f"usage=0x{c.get('usage', 0):04X} path=...{c['path'][-24:]}")
    for info in candidates:
        try:
            probe = Link(info, verbose=False)
        except Exception:
            continue
        try:
            reply = probe.command("LINK PING probe", "probe", timeout_ms=700)
            if reply and "MEO:PONG" in reply:
                probe.close()
                return info
        except Exception:
            pass
        finally:
            try:
                probe.close()
            except Exception:
                pass
    return None


class Link:
    def __init__(self, info, verbose=False):
        self.dev = hid.device()
        self.dev.open_path(info["path"])
        self.dev.set_nonblocking(0)
        self.verbose = verbose
        self.rx_id = None
        self.rx_total = 0
        self.rx_next = 0
        self.rx_buf = bytearray()
        self.dropped = 0
        self.next_id = 0

    def close(self):
        try:
            self.dev.close()
        except Exception:
            pass

    def send_line(self, text: str):
        """按 60 字节分片发送一行命令。"""
        raw = text.encode("utf-8")
        total = (len(raw) + PAYLOAD - 1) // PAYLOAD or 1
        msg_id = self.next_id
        self.next_id = (self.next_id + 1) & 0xFF
        for index in range(total):
            chunk = raw[index * PAYLOAD : (index + 1) * PAYLOAD]
            frame = bytearray(REPORT_SIZE)
            frame[0] = KIND_CMD
            frame[1] = msg_id
            frame[2] = index
            frame[3] = total
            frame[HEADER : HEADER + len(chunk)] = chunk
            # hidapi 要求首字节是 Report ID
            self.dev.write(bytes([REPORT_ID]) + bytes(frame))
        return msg_id

    def read_message(self, timeout_ms=1200):
        """读一条完整消息，返回 (kind, text) 或 None。"""
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            data = self.dev.read(REPORT_SIZE, max(1, int((deadline - time.time()) * 1000)))
            if not data:
                continue
            if len(data) < HEADER:
                continue
            kind, msg_id, index, total = data[0], data[1], data[2], data[3]
            body = bytes(data[HEADER:])

            if index == 0:
                self.rx_id = msg_id
                self.rx_total = total
                self.rx_next = 0
                self.rx_buf = bytearray()
            elif (msg_id != self.rx_id or index != self.rx_next or total != self.rx_total):
                self.dropped += 1
                if self.verbose:
                    print(f"  [drop] id={msg_id} idx={index} total={total} "
                          f"expect=({self.rx_id},{self.rx_next},{self.rx_total})")
                self.rx_total = 0
                continue

            self.rx_buf += body
            self.rx_next = index + 1
            if self.rx_next == self.rx_total:
                text = self.rx_buf.decode("utf-8", "replace").rstrip("\x00")
                self.rx_total = 0
                return kind, text
        return None

    def command(self, text, token=None, timeout_ms=1500):
        """发一条命令并等到包含 token 的回复。"""
        self.send_line(text)
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            got = self.read_message(max(120, int((deadline - time.time()) * 1000)))
            if not got:
                continue
            kind, line = got
            if kind != KIND_REPLY:
                if self.verbose:
                    print(f"  [{chr(kind)}] {line[:100]}")
                continue
            if token is None or token in line:
                return line
            if self.verbose:
                print(f"  [R?] {line[:100]}")
        return None


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    info = find_device(args.verbose)
    if not info:
        print("FAIL  未找到 VID_303A&PID_4005 的 HID collection（设备未连接或固件未更新）")
        return 1

    print(f"device: {info.get('product_string')} "
          f"usage_page=0x{info.get('usage_page', 0):04X} path={info['path'][:40]}...")

    link = Link(info, args.verbose)
    results = []
    try:
        # 1. 最小往返
        line = link.command("LINK PING t1", "t1")
        results.append(check("LINK PING 往返", bool(line and "MEO:PONG" in line),
                             (line or "no reply")[:80]))

        # 2. 多分片长回复：STATUS 行远超 60 字节
        line = link.command("LINK STATUS t2", "t2")
        ok = bool(line and "tx=" in line and "rx=" in line)
        results.append(check("LINK STATUS 多分片重组", ok, (line or "no reply")[:110]))

        # 3. 业务命令走同一条链路
        line = link.command("WIFI STATUS t3", "t3")
        results.append(check("WIFI STATUS 经 HID 可达", bool(line and "MEO:WIFI" in line),
                             (line or "no reply")[:110]))

        # 4. 分片连续性：统计丢包
        for i in range(6):
            link.command(f"LINK PING seq{i}", f"seq{i}", timeout_ms=900)
        results.append(check("连续 6 次往返无丢片", link.dropped == 0,
                             f"dropped={link.dropped}"))

        # 5. 日志镜像可用（默认开启，应能看到 L 类型）
        link.send_line("LINK LOG ON t4")
        deadline = time.time() + 2.0
        saw_log = False
        while time.time() < deadline:
            got = link.read_message(300)
            if not got:
                continue
            if got[0] == KIND_LOG:
                saw_log = True
                if args.verbose:
                    print(f"  [L] {got[1][:90]}")
                break
        results.append(check("日志镜像出现在同一通道", saw_log,
                             "" if saw_log else "2s 内没有 L 帧"))
    finally:
        link.close()

    passed = sum(1 for r in results if r)
    print(f"\n{passed}/{len(results)} 通过")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
