/*
 * 协议层主机单元测试
 *   - CRC-16/ARC 标准向量
 *   - SHA-256 / HMAC-SHA256（FIPS 180-4 / RFC 4231）
 *   - MiWear KDF（与独立 Python 参考实现交叉验证）
 *   - protobuf 命令构造与解析 round-trip
 *   - CRC-32/ISO-HDLC 与 MD5（表盘上传校验，与 zlib / Java MessageDigest 一致）
 *   - 表盘安装协议命令编解码（Gadgetbridge XiaomiWatchfaceService / XiaomiDataUploadService）
 *   - DHCP 应答器（DISCOVER -> OFFER, REQUEST -> ACK）
 *   - IP/TCP/UDP 校验和
 *
 * 编译运行: cd tests && make
 */
#include "crc16.h"
#include "crc32.h"
#include "md5.h"
#include "sha256.h"
#include "proto.h"
#include "iputil.h"
#include "dhcp.h"
#include "miwear.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xw;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("  FAIL: %s\n", what);
    }
}

static void checkEq(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want,
                    const char* what) {
    check(got.size() == want.size() &&
              (got.empty() || std::memcmp(got.data(), want.data(), got.size()) == 0),
          what);
}

static std::string toHex(const uint8_t* d, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += h[d[i] >> 4];
        s += h[d[i] & 0xF];
    }
    return s;
}

// ===================== 测试 =====================

static void testCrc16() {
    std::printf("[crc16]\n");
    // CRC-16/ARC 标准检查值: check("123456789") = 0xBB3D
    const char* s = "123456789";
    uint16_t c = crc16Arc((const uint8_t*)s, 9);
    check(c == 0xBB3D, "crc16 check value 0xBB3D");
    // 空输入
    check(crc16Arc(nullptr, 0) == 0x0000, "crc16 empty = 0");
    // 单字节 0x00
    uint8_t z = 0;
    check(crc16Arc(&z, 1) == 0x0000, "crc16 {00} = 0");
    // 与 AstroBox crc16_arc 交叉验证的向量（"hello"，标准反射 CRC-16/ARC）
    uint16_t h = crc16Arc((const uint8_t*)"hello", 5);
    check(h == 0x34D2, "crc16 hello = 0x34D2");
}

static void testSha256() {
    std::printf("[sha256]\n");
    uint8_t out[32];
    Sha256::hash((const uint8_t*)"", 0, out);
    check(toHex(out, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 empty");

    Sha256::hash((const uint8_t*)"abc", 3, out);
    check(toHex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 abc");

    // 长消息（跨多个块）
    std::string longMsg(1000, 'a');
    Sha256::hash((const uint8_t*)longMsg.data(), longMsg.size(), out);
    check(toHex(out, 32) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "sha256 1000x'a'");

    // HMAC RFC 4231 测试用例 1
    uint8_t key[20];
    std::memset(key, 0x0B, sizeof(key));
    hmacSha256(key, 20, (const uint8_t*)"Hi There", 8, out);
    check(toHex(out, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "hmac rfc4231 case1");

    // HMAC RFC 4231 测试用例 2（key="Jefe", data="what do ya want for nothing?"）
    hmacSha256((const uint8_t*)"Jefe", 4,
               (const uint8_t*)"what do ya want for nothing?", 28, out);
    check(toHex(out, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "hmac rfc4231 case2");

    // 增量 update 与一次性一致（注意：out 已被上面的 1000x'a' 覆盖，
    // 这里重新计算 "abc" 的参考值）
    uint8_t ref[32];
    Sha256::hash((const uint8_t*)"abc", 3, ref);
    Sha256 ctx;
    ctx.reset();
    ctx.update((const uint8_t*)"ab", 2);
    ctx.update((const uint8_t*)"c", 1);
    uint8_t out2[32];
    ctx.final(out2);
    check(std::memcmp(ref, out2, 32) == 0, "sha256 incremental == one-shot");
}

static void testKdf() {
    std::printf("[kdf]\n");
    uint8_t secret[16], phone[16], watch[16];
    for (int i = 0; i < 16; i++) secret[i] = uint8_t(i);
    std::memset(phone, 0x11, 16);
    std::memset(watch, 0x22, 16);

    SessionKeys k;
    kdfMiwear(secret, phone, watch, k);

    // 参考值由独立 Python 实现（hmac/hashlib）计算：
    //   init_key = phone||watch
    //   hmac_key = HMAC-SHA256(init_key, secret)
    //   okm = T1||T2, T_i = HMAC(hmac_key, T_{i-1} || "miwear-auth" || i)
    check(toHex(k.decKey, 16) == "8d77265ea666caeda30d88bbd3ed3449", "kdf decKey");
    check(toHex(k.encKey, 16) == "461a25fe7e56d7b87ff22638a675ba79", "kdf encKey");
    check(toHex(k.decNonce, 4) == "8d70195f", "kdf decNonce");
    check(toHex(k.encNonce, 4) == "77062e83", "kdf encNonce");

    // 手表 HMAC = HMAC-SHA256(decKey, watchNonce || phoneNonce)
    uint8_t confirm[32];
    uint8_t msg[32];
    std::memcpy(msg, watch, 16);
    std::memcpy(msg + 16, phone, 16);
    hmacSha256(k.decKey, 16, msg, 32, confirm);
    check(toHex(confirm, 32) ==
              "a2c40475bcfcb710dfc52b9996d3d3ebf61b593e41081769c63388873983eebd",
          "watch hmac");

    // encryptedNonces = HMAC-SHA256(encKey, phoneNonce || watchNonce)
    uint8_t signs[32];
    std::memcpy(msg, phone, 16);
    std::memcpy(msg + 16, watch, 16);
    hmacSha256(k.encKey, 16, msg, 32, signs);
    check(toHex(signs, 32) ==
              "872aaeae0163fd906057ffe36f2da7c6b598a6c167d381a4efc05ed5d5558817",
          "encryptedNonces");

    // 确定性
    SessionKeys k2;
    kdfMiwear(secret, phone, watch, k2);
    check(std::memcmp(&k, &k2, sizeof(k)) == 0, "kdf deterministic");
}

static void testProto() {
    std::printf("[proto]\n");
    // PhoneNonce 命令: Command{type=1, subtype=26, auth{phoneNonce{nonce}}}
    uint8_t nonce[16];
    for (int i = 0; i < 16; i++) nonce[i] = uint8_t(0xA0 + i);
    auto cmd = buildAuthPhoneNonceCmd(nonce);
    MiCommand parsed = parseCommand(cmd.data(), cmd.size());
    check(parsed.ok && parsed.type == 1 && parsed.subtype == 26, "auth phoneNonce cmd header");
    PbReader authR(parsed.auth.data, parsed.auth.len);
    PbField pn;
    check(authR.find(30, pn), "auth.phoneNonce field 30 present");
    PbReader pnR(pn.data, pn.len);
    std::vector<uint8_t> gotNonce;
    check(pnR.bytesOf(1, gotNonce), "phoneNonce.nonce present");
    checkEq(gotNonce, std::vector<uint8_t>(nonce, nonce + 16), "phoneNonce value");

    // AuthDeviceInfo 明文
    auto di = buildAuthDeviceInfoBytes(33, "ESP32", "CN");
    PbReader diR(di.data(), di.size());
    check(diR.varintOf(1, 99) == 0, "authDeviceInfo.unknown1 = 0");
    check(diR.floatOf(2, -1) == 33.0f, "authDeviceInfo.phoneApiLevel = 33");
    std::string name;
    check(diR.stringOf(3, name) && name == "ESP32", "authDeviceInfo.phoneName");
    check(diR.varintOf(4, 0) == 224, "authDeviceInfo.unknown3 = 224");
    std::string region;
    check(diR.stringOf(5, region) && region == "CN", "authDeviceInfo.region");

    // AuthStep3 命令
    uint8_t signs[32], encInfo[40];
    std::memset(signs, 0x5A, 32);
    std::memset(encInfo, 0x77, 40);
    auto step3 = buildAuthStep3Cmd(signs, encInfo, 40);
    MiCommand p3 = parseCommand(step3.data(), step3.size());
    check(p3.ok && p3.type == 1 && p3.subtype == 27, "auth step3 cmd header");
    PbReader a3(p3.auth.data, p3.auth.len);
    PbField s3;
    check(a3.find(32, s3), "auth.authStep3 field 32");
    PbReader s3r(s3.data, s3.len);
    std::vector<uint8_t> gSigns, gInfo;
    check(s3r.bytesOf(1, gSigns) && gSigns.size() == 32, "authStep3.encryptedNonces");
    checkEq(gSigns, std::vector<uint8_t>(signs, signs + 32), "authStep3 signs value");
    check(s3r.bytesOf(2, gInfo) && gInfo.size() == 40, "authStep3.encryptedDeviceInfo");

    // 天气当前命令
    WeatherNow w;
    w.locationName = "北京";
    w.locationKey = "accu:101010100";
    w.isCurrentLocation = true;
    w.condition = 1;
    w.temperature = 26;
    w.humidity = 40;
    w.windBeaufort = 3;
    w.windDirDeg = 180;
    w.uvIndex = 5;
    w.aqi = 80;
    w.pressure = 1013.25f;
    auto wx = buildWeatherCurrentCmd(w);
    MiCommand pw = parseCommand(wx.data(), wx.size());
    check(pw.ok && pw.type == 10 && pw.subtype == 0, "weather current cmd header");
    PbReader wR(pw.weather.data, pw.weather.len);
    PbField cur;
    check(wR.find(1, cur), "weather.current field 1");
    PbReader curR(cur.data, cur.len);
    check(curR.varintOf(2, 999) == 1, "weather condition");
    PbField temp;
    check(curR.find(3, temp), "weather temperature present");
    PbReader tempR(temp.data, temp.len);
    check(tempR.sintOf(2, -999) == 26, "weather temperature value");

    // 时钟命令
    ClockSet clk{2026, 10, 24, 12, 30, 0, 32, 0, "CST", true};
    auto clkCmd = buildClockCmd(clk);
    MiCommand pc = parseCommand(clkCmd.data(), clkCmd.size());
    check(pc.ok && pc.type == 2 && pc.subtype == 3, "clock cmd header");
    PbReader sysR(pc.system.data, pc.system.len);
    PbField clock;
    check(sysR.find(4, clock), "system.clock field 4");
    PbReader clockR(clock.data, clock.len);
    PbField date;
    check(clockR.find(1, date), "clock.date present");
    PbReader dateR(date.data, date.len);
    check(dateR.varintOf(1, 0) == 2026, "clock year");
    check(dateR.varintOf(2, 0) == 10, "clock month");
    check(dateR.varintOf(3, 0) == 24, "clock day");

    // WMO -> Xiaomi 天气码
    check(wmoToXiaomiCondition(0) == 0, "wmo 0 -> clear");
    check(wmoToXiaomiCondition(3) == 2, "wmo 3 -> overcast");
    check(wmoToXiaomiCondition(61) == 7, "wmo 61 -> light rain");
    check(wmoToXiaomiCondition(95) == 4, "wmo 95 -> thunderstorm");

    // ISO 时间
    std::string iso = unixToIsoWithColons(1790000000); // 2026-09-19T...
    check(iso.size() == 19 && iso[4] == '-' && iso[10] == 'T', "iso format");
}

static void testIputil() {
    std::printf("[iputil]\n");
    // 已知 UDP 校验和向量（RFC 1071 风格）
    // 构造 10.1.10.2:1234 -> 10.1.10.1:53 的 UDP 头
    uint8_t udp[16];
    udp[0] = 0x04; udp[1] = 0xD2;         // sport 1234
    udp[2] = 0x00; udp[3] = 0x35;         // dport 53
    udp[4] = 0x00; udp[5] = 0x10;         // len 16
    udp[6] = 0; udp[7] = 0;
    udp[8] = 0xDE; udp[9] = 0xAD; udp[10] = 0xBE; udp[11] = 0xEF;
    udp[12] = 0x01; udp[13] = 0x02; udp[14] = 0x03; udp[15] = 0x04;
    uint16_t csum = udpChecksum(0x020A010A, 0x010A010A, udp, 16); // 10.1.10.2 -> 10.1.10.1
    check(csum != 0, "udp checksum computed");
    // 把算出的校验和写回后，伪首部+UDP 的一补数校验和应为 0
    udp[6] = uint8_t(csum >> 8);
    udp[7] = uint8_t(csum);
    uint8_t ph[12] = {10, 1, 10, 2, 10, 1, 10, 1, 0, 17, 0, 16};
    uint16_t total = ipChecksumContinue(ipChecksumContinue(0, ph, 12), udp, 16);
    check(uint16_t(~total & 0xFFFF) == 0, "udp checksum self-consistent");

    // IP 分片判断
    uint8_t fragPkt[20] = {0x45, 0, 0, 20, 0, 0, 0x20, 0, 64, 6, 0, 0,
                           10, 1, 10, 2, 93, 184, 216, 34};
    check(ipIsFragment(fragPkt, 20), "MF set -> fragment");
    fragPkt[6] = 0x00; fragPkt[7] = 0x05; // offset=5
    check(ipIsFragment(fragPkt, 20), "nonzero offset -> fragment");
    fragPkt[7] = 0x00;
    check(!ipIsFragment(fragPkt, 20), "no fragment");

    // ipPayload
    const uint8_t* pl;
    size_t plen;
    check(ipPayload(fragPkt, 20, pl, plen) && plen == 0, "ipPayload basic");
    check(!ipPayload(fragPkt, 10, pl, plen), "ipPayload too short");
}

// ---- DHCP 测试辅助：构造手表的 DHCP DISCOVER IP 包 ----
static std::vector<uint8_t> buildDhcpPacket(uint8_t msgType, const uint8_t mac[6],
                                            uint32_t xid) {
    std::vector<uint8_t> dh;
    dh.push_back(1);          // op = BOOTREQUEST
    dh.push_back(1);          // htype = ethernet
    dh.push_back(6);          // hlen
    dh.push_back(0);          // hops
    for (int i = 0; i < 4; i++) dh.push_back(uint8_t(xid >> (8 * (3 - i)))); // xid 大端
    dh.push_back(0); dh.push_back(0);  // secs
    dh.push_back(0x80); dh.push_back(0); // flags = broadcast
    for (int i = 0; i < 4; i++) dh.push_back(0); // ciaddr
    for (int i = 0; i < 4; i++) dh.push_back(0); // yiaddr
    for (int i = 0; i < 4; i++) dh.push_back(0); // siaddr
    for (int i = 0; i < 4; i++) dh.push_back(0); // giaddr
    for (int i = 0; i < 6; i++) dh.push_back(mac[i]); // chaddr
    for (int i = 0; i < 10; i++) dh.push_back(0);    // chaddr padding
    for (int i = 0; i < 64; i++) dh.push_back(0);    // sname
    for (int i = 0; i < 128; i++) dh.push_back(0);   // file
    const uint8_t magic[4] = {0x63, 0x82, 0x53, 0x63};
    for (int i = 0; i < 4; i++) dh.push_back(magic[i]);
    dh.push_back(53); dh.push_back(1); dh.push_back(msgType);
    dh.push_back(55); dh.push_back(4); // parameter request list
    dh.push_back(1); dh.push_back(3); dh.push_back(6); dh.push_back(51);
    dh.push_back(255); // end

    // UDP 68 -> 67
    std::vector<uint8_t> udp;
    udp.push_back(0); udp.push_back(68);
    udp.push_back(0); udp.push_back(67);
    uint16_t ulen = uint16_t(8 + dh.size());
    udp.push_back(uint8_t(ulen >> 8)); udp.push_back(uint8_t(ulen));
    udp.push_back(0); udp.push_back(0);
    udp.insert(udp.end(), dh.begin(), dh.end());

    // IP 0.0.0.0 -> 255.255.255.255
    std::vector<uint8_t> ip;
    ip.push_back(0x45); ip.push_back(0);
    uint16_t total = uint16_t(20 + udp.size());
    ip.push_back(uint8_t(total >> 8)); ip.push_back(uint8_t(total));
    ip.push_back(0); ip.push_back(1);
    ip.push_back(0); ip.push_back(0);
    ip.push_back(64); ip.push_back(17);
    ip.push_back(0); ip.push_back(0);
    for (int i = 0; i < 4; i++) ip.push_back(0);
    ip.push_back(255); ip.push_back(255); ip.push_back(255); ip.push_back(255);
    ip.insert(ip.end(), udp.begin(), udp.end());
    // IP 校验和
    uint16_t csum = ipChecksum(ip.data(), 20);
    ip[10] = uint8_t(csum >> 8);
    ip[11] = uint8_t(csum);
    return ip;
}

static void testDhcp() {
    std::printf("[dhcp]\n");
    uint8_t mac[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    std::vector<uint8_t> disc = buildDhcpPacket(1, mac, 0x12345678); // DISCOVER
    std::vector<uint8_t> reply;
    check(dhcpHandlePacket(disc.data(), disc.size(), reply), "dhcp discover handled");
    check(reply.size() >= 20 + 8 + 240, "dhcp reply size");
    if (reply.size() < 20 + 8 + 240) return;

    // IP 头检查
    check((reply[0] >> 4) == 4, "reply ipv4");
    check(reply[9] == 17, "reply proto udp");
    check(reply[12] == 10 && reply[13] == 1 && reply[14] == 10 && reply[15] == 1,
          "reply src = 10.1.10.1");
    check(reply[16] == 255 && reply[17] == 255 && reply[18] == 255 && reply[19] == 255,
          "reply dst = broadcast");
    // IP 校验和自洽：对含校验和字段的头部求一补数校验和，结果应为 0
    uint16_t ipc = ipChecksum(reply.data(), 20);
    check(ipc == 0, "reply ip checksum valid");

    // UDP 头
    const uint8_t* udp = reply.data() + 20;
    check(udp[0] == 0 && udp[1] == 67, "reply udp sport 67");
    check(udp[2] == 0 && udp[3] == 68, "reply udp dport 68");

    // DHCP 内容
    const uint8_t* dh = reply.data() + 28;
    check(dh[0] == 2, "dhcp op = BOOTREPLY");
    check(dh[4] == 0x12 && dh[5] == 0x34 && dh[6] == 0x56 && dh[7] == 0x78,
          "dhcp xid echoed");
    check(dh[16] == 10 && dh[17] == 1 && dh[18] == 10 && dh[19] == 2,
          "dhcp yiaddr = 10.1.10.2");
    check(dh[20] == 10 && dh[21] == 1 && dh[22] == 10 && dh[23] == 1,
          "dhcp siaddr = 10.1.10.1");
    for (int i = 0; i < 6; i++) check(dh[28 + i] == mac[i], "dhcp chaddr echoed");
    check(dh[236] == 0x63 && dh[237] == 0x82 && dh[238] == 0x53 && dh[239] == 0x63,
          "dhcp magic cookie");

    // 选项：53=OFFER(2), 1=mask, 3=router, 51=lease, 54=server, 6=dns
    const uint8_t* opt = dh + 240;
    size_t oleft = reply.size() - 28 - 240;
    bool hasOffer = false, hasMask = false, hasRouter = false, hasLease = false;
    bool hasServer = false, hasDns = false;
    size_t i = 0;
    while (i + 2 <= oleft) {
        uint8_t code = opt[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }
        uint8_t olen = opt[i + 1];
        if (i + 2 + olen > oleft) break;
        const uint8_t* val = opt + i + 2;
        switch (code) {
            case 53: hasOffer = (olen >= 1 && val[0] == 2); break;
            case 1: hasMask = (olen == 4 && val[0] == 255 && val[3] == 0); break;
            case 3: hasRouter = (olen == 4 && val[0] == 10 && val[3] == 1); break;
            case 51: hasLease = (olen == 4); break;
            case 54: hasServer = (olen == 4 && val[0] == 10 && val[3] == 1); break;
            case 6: hasDns = (olen == 4); break;
        }
        i += 2 + olen;
    }
    check(hasOffer, "dhcp option 53 = OFFER");
    check(hasMask, "dhcp option 1 subnet mask");
    check(hasRouter, "dhcp option 3 router");
    check(hasLease, "dhcp option 51 lease");
    check(hasServer, "dhcp option 54 server id");
    check(hasDns, "dhcp option 6 dns");

    // REQUEST -> ACK
    std::vector<uint8_t> req = buildDhcpPacket(3, mac, 0xAABBCCDD);
    std::vector<uint8_t> ack;
    check(dhcpHandlePacket(req.data(), req.size(), ack), "dhcp request handled");
    if (ack.size() >= 28 + 240) {
        const uint8_t* adh = ack.data() + 28;
        const uint8_t* aopt = adh + 240;
        bool isAck = false;
        size_t j = 0;
        size_t aleft = ack.size() - 28 - 240;
        while (j + 2 <= aleft) {
            if (aopt[j] == 53 && aopt[j + 1] == 1) { isAck = (aopt[j + 2] == 5); break; }
            if (aopt[j] == 255) break;
            if (aopt[j] == 0) { j++; continue; }
            j += 2 + aopt[j + 1];
        }
        check(isAck, "dhcp option 53 = ACK");
        check(adh[4] == 0xAA && adh[5] == 0xBB && adh[6] == 0xCC && adh[7] == 0xDD,
              "dhcp ack xid echoed");
    }

    // 非 DHCP 包不应被处理
    std::vector<uint8_t> notDhcp = disc;
    notDhcp[20 + 2] = 0; notDhcp[20 + 3] = 80; // UDP dport 80
    std::vector<uint8_t> out2;
    check(!dhcpHandlePacket(notDhcp.data(), notDhcp.size(), out2), "non-dhcp ignored");
}

// ===================== CRC-32 / MD5 =====================

static void testCrc32() {
    std::printf("[crc32]\n");
    // 标准检查值（zlib crc32 / CRC-32/ISO-HDLC）
    check(Crc32::hash((const uint8_t*)"", 0) == 0x00000000u, "crc32 empty = 0");
    check(Crc32::hash((const uint8_t*)"123456789", 9) == 0xCBF43926u, "crc32 check value 0xCBF43926");
    check(Crc32::hash((const uint8_t*)"hello", 5) == 0x3610A686u, "crc32 hello = 0x3610A686");
    // 流式 == 一次性（跨 update 边界，含 64 字节缓冲块边界）
    const char* s = "The quick brown fox jumps over the lazy dog";
    size_t n = std::strlen(s);
    uint32_t one = Crc32::hash((const uint8_t*)s, n);
    for (size_t chunk : {1u, 7u, 64u, 65u, 200u}) {
        Crc32 c;
        size_t off = 0;
        while (off < n) {
            size_t k = (off + chunk <= n) ? chunk : (n - off);
            c.update((const uint8_t*)s + off, k);
            off += k;
        }
        check(c.final() == one, "crc32 streaming == one-shot");
    }
    // final() 不破坏状态（表盘上传场景: 边扫边算）
    Crc32 c;
    c.update((const uint8_t*)"12345", 5);
    uint32_t f1 = c.final();
    c.update((const uint8_t*)"6789", 4);
    check(c.final() == 0xCBF43926u && f1 != 0xCBF43926u, "crc32 final() non-destructive");
}

static void testMd5() {
    std::printf("[md5]\n");
    struct Vec { const char* in; const char* hex; };
    static const Vec vecs[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
         "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"123456789012345678901234567890123456789012345678901234567890"
         "12345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (const auto& v : vecs) {
        uint8_t out[16];
        Md5::hash((const uint8_t*)v.in, std::strlen(v.in), out);
        check(toHex(out, 16) == v.hex, v.hex);
    }
    // 流式 == 一次性（跨 64 字节块边界）
    const char* s = vecs[5].in;
    size_t n = std::strlen(s);
    uint8_t ref[16];
    Md5::hash((const uint8_t*)s, n, ref);
    for (size_t chunk : {1u, 13u, 55u, 56u, 64u, 65u, 200u}) {
        Md5 m;
        size_t off = 0;
        while (off < n) {
            size_t k = (off + chunk <= n) ? chunk : (n - off);
            m.update((const uint8_t*)s + off, k);
            off += k;
        }
        uint8_t out[16];
        m.final(out);
        check(std::memcmp(out, ref, 16) == 0, "md5 streaming == one-shot");
    }
    // reset 后可复用
    Md5 m;
    uint8_t a[16], b[16];
    m.update((const uint8_t*)"abc", 3);
    m.final(a);
    m.reset();
    m.update((const uint8_t*)"abc", 3);
    m.final(b);
    check(std::memcmp(a, b, 16) == 0 && toHex(a, 16) == "900150983cd24fb0d6963f7d28e17f72",
          "md5 reset reusable");
}

// ===================== 表盘协议 =====================

// 手工构造 Command{type=4, subtype=4, watchface{installStatus=N}}
static std::vector<uint8_t> makeInstallStatusCmd(uint32_t status) {
    std::vector<uint8_t> wf;
    pbPutVarintField(wf, 5, status);
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, 4);
    pbPutVarintField(out, 2, 4);
    pbPutMessageField(out, 6, wf);
    return out;
}

// 手工构造 Command{type=22, subtype=0, dataUpload{dataUploadAck{...}}}
static std::vector<uint8_t> makeUploadAckCmd(uint32_t unknown2, uint32_t resume,
                                             bool hasChunk, uint32_t chunk) {
    std::vector<uint8_t> ack;
    if (unknown2) pbPutVarintField(ack, 2, unknown2);
    if (resume) pbPutVarintField(ack, 4, resume);
    if (hasChunk) pbPutVarintField(ack, 5, chunk);
    std::vector<uint8_t> du;
    pbPutMessageField(du, 2, ack);
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, 22);
    pbPutVarintField(out, 2, 0);
    pbPutMessageField(out, 24, du);
    return out;
}

// 手工构造 Command{type=4, subtype=0, watchface{watchfaceList{...}}}
static std::vector<uint8_t> makeWatchfaceListCmd() {
    auto info = [](const char* id, const char* name, bool active, bool canDelete) {
        std::vector<uint8_t> e;
        pbPutStringField(e, 1, id);
        pbPutStringField(e, 2, name);
        pbPutVarintField(e, 3, active ? 1 : 0);
        pbPutVarintField(e, 4, canDelete ? 1 : 0);
        return e;
    };
    std::vector<uint8_t> list;
    pbPutMessageField(list, 1, info("1001", "经典", true, false));
    pbPutMessageField(list, 1, info("2002", "炫酷", false, true));
    std::vector<uint8_t> wf;
    pbPutMessageField(wf, 1, list);
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, 4);
    pbPutVarintField(out, 2, 0);
    pbPutMessageField(out, 6, wf);
    return out;
}

static void testWatchfaceProto() {
    std::printf("[watchface proto]\n");

    // ---- buildWatchfaceListCmd: Command{type=4, subtype=0}
    auto listCmd = buildWatchfaceListCmd();
    MiCommand lc = parseCommand(listCmd.data(), listCmd.size());
    check(lc.ok && lc.type == 4 && lc.subtype == 0, "watchfaceList cmd header");

    // ---- buildWatchfaceInstallStartCmd: {id="1001", size=123456}
    auto startCmd = buildWatchfaceInstallStartCmd("1001", 123456);
    MiCommand sc = parseCommand(startCmd.data(), startCmd.size());
    check(sc.ok && sc.type == 4 && sc.subtype == 4, "installStart cmd header");
    PbField start;
    check(sc.watchface.wire == 2 && PbReader(sc.watchface.data, sc.watchface.len).subReader(6, start),
          "installStart.watchfaceInstallStart field 6");
    if (start.wire == 2) {
        std::string id;
        PbReader sr(start.data, start.len);
        check(sr.stringOf(1, id) && id == "1001", "installStart.id = 1001");
        check(sr.varintOf(2, 0) == 123456, "installStart.size = 123456");
    }

    // ---- buildWatchfaceSetCmd / DeleteCmd: watchface{watchfaceId}
    for (uint32_t expectSub : {cmd::WATCHFACE_SET, cmd::WATCHFACE_DELETE}) {
        auto c = (expectSub == cmd::WATCHFACE_SET) ? buildWatchfaceSetCmd("2002")
                                                   : buildWatchfaceDeleteCmd("2002");
        MiCommand p = parseCommand(c.data(), c.size());
        check(p.ok && p.type == 4 && p.subtype == expectSub, "watchface set/delete header");
        std::string id;
        PbReader wr(p.watchface.data, p.watchface.len);
        check(wr.stringOf(2, id) && id == "2002", "watchfaceId = 2002");
    }

    // ---- buildDataUploadRequestCmd: type=16, md5, size
    uint8_t md5[16];
    Md5::hash((const uint8_t*)"123456789", 9, md5);
    auto upCmd = buildDataUploadRequestCmd(cmd::UPLOAD_TYPE_WATCHFACE, md5, 1048576);
    MiCommand uc = parseCommand(upCmd.data(), upCmd.size());
    check(uc.ok && uc.type == 22 && uc.subtype == 0, "dataUpload cmd header");
    PbField req;
    check(uc.dataUpload.wire == 2 && PbReader(uc.dataUpload.data, uc.dataUpload.len).subReader(1, req),
          "dataUpload.dataUploadRequest field 1");
    if (req.wire == 2) {
        std::vector<uint8_t> gotMd5;
        PbReader rr(req.data, req.len);
        check(rr.varintOf(1, 0) == 16, "dataUploadRequest.type = 16");
        check(rr.bytesOf(2, gotMd5) && gotMd5.size() == 16 &&
                  std::memcmp(gotMd5.data(), md5, 16) == 0, "dataUploadRequest.md5sum");
        check(rr.varintOf(3, 0) == 1048576, "dataUploadRequest.size = 1MB");
    }

    // ---- parseWatchfaceInstallStatus
    auto isc = makeInstallStatusCmd(0);
    MiCommand ip = parseCommand(isc.data(), isc.size());
    WatchfaceInstallStatus st = parseWatchfaceInstallStatus(ip.watchface);
    check(st.valid && st.installStatus == 0, "installStatus 0 = can upload");
    auto isc2 = makeInstallStatusCmd(2);
    MiCommand ip2 = parseCommand(isc2.data(), isc2.size());
    WatchfaceInstallStatus st2 = parseWatchfaceInstallStatus(ip2.watchface);
    check(st2.valid && st2.installStatus == 2, "installStatus 2 = already installed");
    // 非 watchface 字段不应误判
    check(!parseWatchfaceInstallStatus(ip2.system).valid, "installStatus on wrong field invalid");

    // ---- parseDataUploadAck
    auto ack1 = makeUploadAckCmd(0, 0, false, 0);
    MiCommand ap = parseCommand(ack1.data(), ack1.size());
    DataUploadAck a = parseDataUploadAck(ap.dataUpload);
    check(a.valid && a.unknown2 == 0 && a.resumePosition == 0 && !a.hasChunkSize,
          "dataUploadAck defaults (chunkSize absent)");
    auto ack2 = makeUploadAckCmd(0, 0, true, 4096);
    MiCommand ap2 = parseCommand(ack2.data(), ack2.size());
    DataUploadAck a2 = parseDataUploadAck(ap2.dataUpload);
    check(a2.valid && a2.hasChunkSize && a2.chunkSize == 4096, "dataUploadAck chunkSize 4096");
    auto ack3 = makeUploadAckCmd(0, 128, false, 0);
    MiCommand ap3 = parseCommand(ack3.data(), ack3.size());
    DataUploadAck a3 = parseDataUploadAck(ap3.dataUpload);
    check(a3.valid && a3.resumePosition == 128, "dataUploadAck resumePosition parsed");
    check(!parseDataUploadAck(ap3.watchface).valid, "dataUploadAck on wrong field invalid");

    // ---- parseWatchfaceList
    auto wl = makeWatchfaceListCmd();
    MiCommand wp = parseCommand(wl.data(), wl.size());
    std::vector<WatchfaceEntry> entries;
    check(parseWatchfaceList(wp.watchface, entries), "parseWatchfaceList ok");
    check(entries.size() == 2, "watchface list 2 entries");
    if (entries.size() == 2) {
        check(entries[0].id == "1001" && entries[0].name == "经典" &&
                  entries[0].active && !entries[0].canDelete, "entry[0] 1001 classic active");
        check(entries[1].id == "2002" && entries[1].name == "炫酷" &&
                  !entries[1].active && entries[1].canDelete, "entry[1] 2002 deletable");
    }
    // 空列表: watchface{watchfaceList{}}
    std::vector<uint8_t> emptyListBytes;
    {
        std::vector<uint8_t> emptyList;
        pbPutMessageField(emptyListBytes, 1, emptyList);
    }
    PbField emptyWf{1, 2, 0, emptyListBytes.data(), emptyListBytes.size()};
    std::vector<WatchfaceEntry> none;
    check(parseWatchfaceList(emptyWf, none) && none.empty(), "empty watchface list");
    // 完全无 watchface 字段
    std::vector<WatchfaceEntry> none2;
    check(!parseWatchfaceList(ap.watchface, none2), "watchfaceList on wrong field invalid");
}

int main() {
    testCrc16();
    testCrc32();
    testSha256();
    testMd5();
    testKdf();
    testProto();
    testWatchfaceProto();
    testIputil();
    testDhcp();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
