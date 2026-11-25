/* ESP32-S31 Web Serial transport.
 *
 * esptool-js' stock transport keeps one reader for the whole session and its
 * read() can miss a wake-up when a packet is split across USB transfers. The
 * S31 ROM/stub is much chattier than the S3, so this transport owns the read
 * loop explicitly and decodes SLIP frames with a persistent input buffer.
 */
import { Transport } from './esptool-js-0.7.0.js';

function join(a, b) {
  if (!a.length) return b;
  if (!b.length) return a;
  const out = new Uint8Array(a.length + b.length);
  out.set(a);
  out.set(b, a.length);
  return out;
}

export class S31Transport extends Transport {
  constructor(device) {
    super(device, false, true);
    this.s31Running = false;
    this.s31InputError = null;
    this.s31Wake = null;
    this.s31Reading = false;
  }

  async readLoop() {
    if (this.s31Running) throw new Error('串口接收任务重复启动。');
    this.s31Running = true;
    this.s31InputError = null;
    let reader;
    try {
      reader = this.reader = this.device.readable.getReader();
      while (this.s31Running) {
        const { value, done } = await reader.read();
        if (done) throw new Error('串口接收已结束。');
        if (value && value.length) {
          if (this.buffer.length + value.length > 0x100000) {
            throw new Error('串口接收超过安全缓冲范围。');
          }
          this.buffer = join(this.buffer, Uint8Array.from(value));
          if (this.s31Wake) this.s31Wake();
        }
      }
    } catch (error) {
      this.s31InputError = error;
      if (this.s31Wake) this.s31Wake();
    } finally {
      if (reader) reader.releaseLock();
      if (this.reader === reader) this.reader = undefined;
      this.s31Running = false;
    }
  }

  waitForInput(timeout) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.s31Wake = null;
        reject(new Error('串口接收超时。'));
      }, timeout);
      this.s31Wake = () => {
        clearTimeout(timer);
        this.s31Wake = null;
        resolve();
      };
    });
  }

  async read(timeout) {
    if (this.s31Reading) throw new Error('串口读取重叠。');
    this.s31Reading = true;
    let started = false;
    let escaped = false;
    let packet = new Uint8Array(0);
    try {
      for (;;) {
        if (this.s31InputError) {
          throw new Error(`串口输入错误：${this.s31InputError.message}`);
        }
        if (!this.buffer.length) {
          await this.waitForInput(timeout);
          continue;
        }
        const bytes = this.buffer;
        this.buffer = new Uint8Array(0);
        for (let i = 0; i < bytes.length; i += 1) {
          const byte = bytes[i];
          if (!started) {
            if (byte !== 0xc0) throw new Error(`SLIP 包头异常：0x${byte.toString(16)}。`);
            started = true;
          } else if (escaped) {
            escaped = false;
            if (byte === 0xdc) packet = join(packet, Uint8Array.of(0xc0));
            else if (byte === 0xdd) packet = join(packet, Uint8Array.of(0xdb));
            else throw new Error('SLIP 转义异常。');
          } else if (byte === 0xdb) {
            escaped = true;
          } else if (byte === 0xc0) {
            if (!packet.length) continue;
            if (i + 1 < bytes.length) {
              this.buffer = join(bytes.subarray(i + 1), this.buffer);
            }
            return packet;
          } else {
            packet = join(packet, Uint8Array.of(byte));
          }
        }
      }
    } finally {
      this.s31Reading = false;
    }
  }

  async disconnect() {
    this.s31Running = false;
    if (this.s31Wake) this.s31Wake();
    await super.disconnect();
  }
}
