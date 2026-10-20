// HD2 Offline — Local Gateway Server (v2: learning proxy).
// Listens on 127.0.0.1:443 (TLS, multi-SAN self-signed cert) and :80 (plain).
// - Serves fakes: PlayFab LoginWithSteam, Arrowhead war API (real snapshots)
// - PROXIES pss-cloud.net requests to the REAL Sony servers (DNS via 8.8.8.8 to
//   bypass the hosts file), logging full request/response for the offline replay set
// - Logs every request the game makes
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;

class Gateway {
    static X509Certificate2 cert;
    static readonly string logFile = @"C:\HD2VR\phase1\gateway\gateway.log";
    static readonly string dataDir = @"C:\HD2VR\phase1\gateway\data";
    static readonly string replayDir = @"C:\HD2VR\phase1\gateway\replay";
    static readonly object logLock = new object();
    // v7.6 crash-repro config: PSN only proxied; PlayFab + pubsub LOCAL; api.live.prod piped (real login).
    static readonly List<string> ProxyHosts = new List<string> { "pss-cloud.net" };

    const string PlayFabLoginJson = @"{""code"":200,""status"":""OK"",""data"":{" +
        @"""SessionTicket"":""HD2LOCAL-3F2504E04F8911D39A0C0305E82C3301-LOCALOFFLINE""," +
        @"""PlayFabId"":""J0HNHELLD1VER000000000001"",""NewlyCreated"":false," +
        @"""SettingsForUser"":{""NeedsAttribution"":false}," +
        @"""LastLoginTime"":""2026-10-16T00:00:00Z""," +
        @"""UserAccountInfo"":{" +
        @"""PlayFabId"":""J0HNHELLD1VER000000000001""," +
        @"""Created"":""2024-02-20T20:58:22Z""," +
        @"""Username"":""JohnHelldiver""," +
        @"""TitleInfo"":{""DisplayName"":""John Helldiver"",""Created"":""2024-02-20T20:58:22Z"",""LastLogin"":""2026-10-16T00:00:00Z"",""FirstLogin"":""2024-02-20T20:58:22Z""}," +
        @"""SteamInfo"":{""SteamId"":""76561190000000000"",""SteamCountry"":""US""}}," +
        @"""PlayerProfile"":{" +
        @"""PlayerId"":""J0HNHELLD1VER000000000001""," +
        @"""DisplayName"":""John Helldiver""," +
        @"""Locations"":[{""CountryCode"":""US""}]}," +
        @"""EntityToken"":{""EntityToken"":""HD2LOCALENTITYTOKEN-0001""," +
        @"""Entity"":{""Id"":""J0HNHELLD1VER000000000001"",""Type"":""title_player_account""}," +
        @"""TokenExpiration"":""2030-01-01T00:00:00Z""}}}";

    static string ArrowheadLoginJson; // loaded from file in Main()

    static void Log(string s) {
        lock (logLock) {
            try { File.AppendAllText(logFile, DateTime.Now.ToString("HH:mm:ss.fff") + " " + s + Environment.NewLine); } catch { }
            Console.WriteLine(s);
        }
    }

    static void Main() {
        string tp = File.ReadAllText(@"C:\HD2VR\phase1\gateway\cert-thumbprint.txt").Trim();
        var store = new X509Store(StoreName.My, StoreLocation.LocalMachine);
        store.Open(OpenFlags.ReadOnly);
        cert = store.Certificates.Find(X509FindType.FindByThumbprint, tp, false)[0];
        store.Close();
        Directory.CreateDirectory(replayDir);
        ArrowheadLoginJson = File.ReadAllText(@"C:\HD2VR\phase1\gateway\arrowhead-login.json");
        Log("gateway v7 starting (John Helldiver fake login enabled)");

        var t443 = new Thread(() => Listen(443, true));
        t443.IsBackground = true; t443.Start();
        Listen(80, false);
    }

    static void Listen(int port, bool tls) {
        var listener = new TcpListener(IPAddress.Any, port);
        listener.Start();
        Log("listening on port " + port + (tls ? " (TLS)" : " (plain)"));
        while (true) {
            try {
                var tcp = listener.AcceptTcpClient();
                var t = new Thread(() => Handle(tcp, tls));
                t.IsBackground = true; t.Start();
            } catch (Exception ex) { Log("accept fail p" + port + ": " + ex.Message); }
        }
    }

    // ---- SNI-based TCP passthrough: let the game do TLS directly with real Arrowhead ----
    class PrefixStream : Stream {
        byte[] prefix; int pos; Stream inner;
        public PrefixStream(byte[] p, Stream i) { prefix = p; inner = i; }
        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return true; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override void Flush() { inner.Flush(); }
        public override int Read(byte[] buf, int offset, int count) {
            if (pos < prefix.Length) { int n = Math.Min(count, prefix.Length - pos); Array.Copy(prefix, pos, buf, offset, n); pos += n; return n; }
            return inner.Read(buf, offset, count);
        }
        public override long Seek(long o, SeekOrigin so) { throw new NotSupportedException(); }
        public override void SetLength(long v) { throw new NotSupportedException(); }
        public override void Write(byte[] buf, int offset, int count) { inner.Write(buf, offset, count); }
    }

    static byte[] ReadClientHello(NetworkStream ns) {
        try {
            byte[] buf = new byte[16384]; int n = 0;
            while (n < 5) { int r = ns.Read(buf, n, buf.Length - n); if (r <= 0) return null; n += r; }
            int recLen = (buf[3] << 8) | buf[4];
            while (n < 5 + recLen && n < buf.Length) { int r = ns.Read(buf, n, buf.Length - n); if (r <= 0) break; n += r; }
            byte[] res = new byte[n]; Array.Copy(buf, res, n); return res;
        } catch { return null; }
    }

    static string ParseSni(byte[] b) {
        try {
            if (b == null || b.Length < 10 || b[0] != 0x16) return null;
            int i = 5;
            if (b[i] != 0x01) return null;
            i += 4;            // handshake header
            i += 34;           // version + random
            int sidLen = b[i]; i += 1 + sidLen;
            int csLen = (b[i] << 8) | b[i + 1]; i += 2 + csLen;
            int cmLen = b[i]; i += 1 + cmLen;
            if (i + 2 > b.Length) return null;
            int extLen = (b[i] << 8) | b[i + 1]; i += 2;
            int extEnd = Math.Min(i + extLen, b.Length);
            while (i + 4 <= extEnd) {
                int type = (b[i] << 8) | b[i + 1]; int el = (b[i + 2] << 8) | b[i + 3]; i += 4;
                if (type == 0x0000 && i + 5 <= extEnd) {
                    int j = i + 2;                       // skip list length
                    int nlen = (b[j + 1] << 8) | b[j + 2];
                    return Encoding.ASCII.GetString(b, j + 3, nlen);
                }
                i += el;
            }
        } catch { }
        return null;
    }

    static string _arrowheadIp;
    static void TcpPipe(TcpClient tcp, NetworkStream client, byte[] prefix) {
        try {
            if (_arrowheadIp == null) {
                _arrowheadIp = ResolveViaGoogle("api.live.prod.thehelldiversgame.com");
                if (_arrowheadIp == null) _arrowheadIp = "172.200.240.16";
            }
            var srv = new TcpClient();
            srv.Connect(_arrowheadIp, 443);
            var ss = srv.GetStream();
            ss.Write(prefix, 0, prefix.Length); ss.Flush();
            var t1 = new Thread(() => { long sent = 0; try { var b = new byte[65536]; int n; while ((n = client.Read(b, 0, b.Length)) > 0) { ss.Write(b, 0, n); sent += n; } } catch { } Log("~ pipe -> sent " + sent + "b"); try { srv.Close(); } catch { } });
            var t2 = new Thread(() => { long got = 0; try { var b = new byte[65536]; int n; while ((n = ss.Read(b, 0, b.Length)) > 0) { client.Write(b, 0, n); got += n; } } catch { } Log("~ pipe <- recv " + got + "b"); try { tcp.Close(); } catch { } });
            t1.IsBackground = true; t2.IsBackground = true; t1.Start(); t2.Start();
            t1.Join(); t2.Join();
        } catch (Exception ex) { Log("~ pipethread error: " + ex.Message); }
    }

    static void Handle(TcpClient tcp, bool tls) {
        try {
            Stream s = tcp.GetStream();
            if (tls) {
                var ns = (NetworkStream)s;
                byte[] prefix = ReadClientHello(ns);
                string sni = ParseSni(prefix);
                if (sni != null && sni.IndexOf("api.live.prod", StringComparison.OrdinalIgnoreCase) >= 0 && File.Exists(@"C:\HD2VR\phase1\gateway\passthrough.flag")) {
                    Log("~ sni passthrough: " + sni);
                    TcpPipe(tcp, ns, prefix);
                    try { tcp.Close(); } catch { }
                    return;
                }
                var ps = new PrefixStream(prefix, s);
                var ssl = new SslStream(ps, false);
                ssl.AuthenticateAsServer(cert, false, SslProtocols.Tls | SslProtocols.Tls11 | SslProtocols.Tls12, false);
                s = ssl;
            }
            tcp.ReceiveTimeout = 15000; tcp.SendTimeout = 15000;
            while (true) {
                var req = ReadRequest(s);
                if (req == null) break;
                // RAW PIPE disabled (flaky + superseded by fake login): gated behind rawpipe.flag
                if (File.Exists(@"C:\HD2VR\phase1\gateway\rawpipe.flag") && req.Path.StartsWith("/api/Account/Login") && (req.Headers.ContainsKey("host") && req.Headers["host"].Contains("api.live.prod"))) {
                    byte[] rawResp = RawPipeLogin(req);
                    if (rawResp != null) {
                        s.Write(rawResp, 0, rawResp.Length); s.Flush();
                        continue;
                    }
                    Log("~ rawpipe failed - falling back to local fake");
                }
                // WebSocket upgrade (SignalR pubsub): 101 + minimal SignalR handshake
                if (req.Headers.ContainsKey("upgrade") && req.Headers["upgrade"].ToLower().Contains("websocket")) {
                    string wsKey = req.Headers.ContainsKey("sec-websocket-key") ? req.Headers["sec-websocket-key"] : "";
                    string acc = Convert.ToBase64String(System.Security.Cryptography.SHA1.Create().ComputeHash(Encoding.ASCII.GetBytes(wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
                    string r101 = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + "\r\n\r\n";
                    byte[] b101 = Encoding.ASCII.GetBytes(r101);
                    s.Write(b101, 0, b101.Length); s.Flush();
                    Log("~ ws upgraded (SignalR)");
                    WsLoop(s);
                    break;
                }
                int status; string body;
                Route(req, out status, out body);
                byte[] b = Encoding.UTF8.GetBytes(body);
                string head = "HTTP/1.1 " + status + " " + (status == 200 ? "OK" : "Status") +
                    "\r\nContent-Type: application/json\r\nContent-Length: " + b.Length +
                    "\r\nConnection: keep-alive\r\n\r\n";
                byte[] hb = Encoding.ASCII.GetBytes(head);
                s.Write(hb, 0, hb.Length);
                s.Write(b, 0, b.Length);
                s.Flush();
            }
        } catch (Exception ex) { Log("conn end: " + ex.Message); }
        try { tcp.Close(); } catch { }
    }

    class Req { public string Method, Path; public Dictionary<string,string> Headers = new Dictionary<string,string>(); public List<string> RawHeaders = new List<string>(); public string Body = ""; public byte[] RawBytes; }

    static Req ReadRequest(Stream s) {
        var buf = new byte[262144];
        int n = 0;
        int headerEnd;
        while (true) {
            if (n >= buf.Length) return null;
            int r = s.Read(buf, n, buf.Length - n);
            if (r <= 0) return null;
            n += r;
            string t = Encoding.ASCII.GetString(buf, 0, n);
            headerEnd = t.IndexOf("\r\n\r\n");
            if (headerEnd >= 0) break;
        }
        string head = Encoding.ASCII.GetString(buf, 0, headerEnd);
        var lines = head.Split(new[] { "\r\n" }, StringSplitOptions.None);
        var req = new Req();
        var parts = lines[0].Split(' ');
        if (parts.Length < 2) return null;
        req.Method = parts[0]; req.Path = parts[1];
        for (int i = 1; i < lines.Length; i++) {
            int c = lines[i].IndexOf(':');
            if (c > 0) {
                req.Headers[lines[i].Substring(0, c).Trim().ToLower()] = lines[i].Substring(c + 1).Trim();
                req.RawHeaders.Add(lines[i]);
            }
        }
        int contentLen = 0;
        if (req.Headers.ContainsKey("content-length")) int.TryParse(req.Headers["content-length"], out contentLen);
        int bodyHave = n - headerEnd - 4;
        while (bodyHave < contentLen) {
            int r = s.Read(buf, n, buf.Length - n);
            if (r <= 0) break;
            n += r; bodyHave = n - headerEnd - 4;
        }
        if (contentLen > 0) req.Body = Encoding.UTF8.GetString(buf, headerEnd + 4, Math.Min(contentLen, bodyHave > 0 ? bodyHave : 0));
        req.RawBytes = new byte[n]; Array.Copy(buf, req.RawBytes, n);
        string host = req.Headers.ContainsKey("host") ? req.Headers["host"] : "";
        bool isProxy = false;
        foreach (var ph in ProxyHosts) if (string.Equals(ph, host, StringComparison.OrdinalIgnoreCase)) { isProxy = true; break; }
        string allHeaders = "";
        if (isProxy || host.Contains("api.live.prod") || host.Contains("playfabapi")) {
            foreach (var kv in req.Headers) allHeaders += "\n    " + kv.Key + ": " + (kv.Value.Length > 200 ? kv.Value.Substring(0, 200) + "..." : kv.Value);
            // RAW dump for deep diagnosis
            try {
                string raw = Encoding.ASCII.GetString(buf, 0, headerEnd) + "\r\n[BODY " + contentLen + " bytes]\r\n" + req.Body;
                File.AppendAllText(@"C:\HD2VR\phase1\gateway\rawdump.log", "===== " + DateTime.Now.ToString("HH:mm:ss.fff") + " =====\r\n" + raw + "\r\n\r\n");
            } catch { }
        }
        Log("> " + req.Method + " " + req.Path + "  host=" + host +
            "  ua=" + (req.Headers.ContainsKey("user-agent") ? req.Headers["user-agent"].Substring(0, Math.Min(40, req.Headers["user-agent"].Length)) : "?") +
            (allHeaders.Length > 0 ? "  HEADERS:" + allHeaders : "") +
            (req.Body.Length > 0 ? "  body(" + req.Body.Length + ")=" + req.Body.Substring(0, Math.Min(300, req.Body.Length)) : ""));
        return req;
    }

    // ---- DNS via 8.8.8.8 (bypasses the hosts file) ----
    static readonly byte[] DnsServer = { 8, 8, 8, 8 };
    static readonly Dictionary<string, string[]> IpFallback = new Dictionary<string, string[]> {
        { "pss-cloud.net", new string[] { "44.233.151.84", "54.244.195.76", "44.245.124.232" } },
        { "974a9.playfabapi.com", new string[] { "150.171.109.66", "150.171.110.17" } },
        { "api.live.prod.thehelldiversgame.com", new string[] { "172.200.240.16", "68.154.15.59" } }
    };
    static string ResolveRealIp(string host) {        string[] list = null;
        if (IpFallback.ContainsKey(host)) list = IpFallback[host];
        if (list != null && list.Length > 0) {
            // rotate through the list (persist index across calls via static)
            int idx = _rrIndex % list.Length; _rrIndex++;
            return list[idx];
        }
        try { return ResolveViaGoogle(host); } catch { return null; }
    }
    static int _rrIndex = 0;
    static string ResolveViaGoogle(string host) {
        try {
            var udp = new UdpClient();
            udp.Connect(new IPEndPoint(new IPAddress(DnsServer), 53));
            udp.Client.ReceiveTimeout = 5000;
            // build query
            var q = new List<byte>();
            q.AddRange(new byte[] { 0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
            foreach (var label in host.Split('.')) {
                q.Add((byte)label.Length);
                q.AddRange(Encoding.ASCII.GetBytes(label));
            }
            q.AddRange(new byte[] { 0x00, 0x00, 0x01, 0x00, 0x01 }); // A, IN
            var qb = q.ToArray();
            udp.Send(qb, qb.Length);
            var ep = new IPEndPoint(IPAddress.Any, 0);
            byte[] resp = udp.Receive(ref ep);
            // skip header(12) + question
            int i = 12;
            while (i < resp.Length && resp[i] != 0) i += resp[i] + 1; i++; // skip qname + null label
            i += 4; // QTYPE(2) + QCLASS(2)
            // answers
            int answers = (resp[7] << 8) | resp[6];
            for (int a = 0; a < answers; a++) {
                // name (could be pointer)
                if ((resp[i] & 0xC0) != 0) i += 2; else { while (resp[i] != 0) i += resp[i] + 1; i++; }
                int type = (resp[i] << 8) | resp[i + 1];
                i += 8; // type(2)+class(2)+ttl(4)
                int rdlen = (resp[i] << 8) | resp[i + 1];
                i += 2;
                if (type == 1 && rdlen == 4) {
                    return resp[i] + "." + resp[i + 1] + "." + resp[i + 2] + "." + resp[i + 3];
                }
                i += rdlen;
            }
        } catch (Exception ex) { Log("dns fail " + host + ": " + ex.Message); }
        return null;
    }

    // ---- PSN token memory (fixes the SDK's auth/config race) ----
    static string LastPsnToken = null;

    // ---- proxy to the real server via curl (ALPN/TLS-capable; .NET Framework SslStream cannot send ALPN) ----
    static bool ProxyReal(Req r, out int status, out string body) {
        status = 200; body = "{}";
        string host = r.Headers.ContainsKey("host") ? r.Headers["host"] : null;
        if (host == null) return false;
        string ip = ResolveRealIp(host);
        if (ip == null) { Log("~ proxy: no ip for " + host); return false; }
        try {
            string tmp = Path.Combine(Path.GetTempPath(), "hd2gw_" + Guid.NewGuid().ToString("N") + ".json");
            File.WriteAllText(tmp, r.Body);
            var args = new StringBuilder();
            args.Append("-s --http1.1 --compressed --max-time 12 --resolve " + host + ":443:" + ip);
            args.Append(" -X " + r.Method);
            args.Append(" \"https://" + host + r.Path + "\"");
            string[] skip = { "connection", "content-length", "host", "keep-alive", "transfer-encoding", "proxy-connection", "upgrade" };
            // forward RAW header lines (original casing preserved - Arrowhead's X-Signature/Key_ID validation may be case-sensitive)
            foreach (string rh in r.RawHeaders) {
                int c = rh.IndexOf(':');
                if (c <= 0) continue;
                string name = rh.Substring(0, c).Trim().ToLower();
                if (Array.IndexOf(skip, name) >= 0) continue;
                string val = rh.Substring(c + 1).Trim();
                if (name == "authorization" && (val.Trim().Equals("Bearer") || val.Length == 0)) {
                    if (LastPsnToken != null) { Log("~ injecting remembered PSN token for " + r.Path); val = "Bearer " + LastPsnToken; }
                    else continue;
                }
                args.Append(" -H \"" + rh.Substring(0, c).Trim() + ": " + val.Replace("\"", "'") + "\"");
            }
            if (r.Body.Length > 0) args.Append(" --data-binary \"@" + tmp + "\"");
            args.Append(" -w \"\\n__STATUS__%{http_code}\"");
            var psi = new ProcessStartInfo(@"C:\Windows\System32\curl.exe", args.ToString()) {
                RedirectStandardOutput = true, RedirectStandardError = true,
                UseShellExecute = false, CreateNoWindow = true
            };
            var proc = Process.Start(psi);
            string outp = proc.StandardOutput.ReadToEnd();
            proc.WaitForExit(20000);
            try { File.Delete(tmp); } catch { }
            int st = outp.LastIndexOf("__STATUS__");
            if (st >= 0) {
                string sc = outp.Substring(st + 10).Trim();
                int.TryParse(sc, out status);
                body = outp.Substring(0, st).TrimEnd('\r', '\n');
            } else { body = outp; }
            Log("~ PROXIED(curl) " + host + r.Path + " -> " + status + "  body(" + body.Length + ")=" + body.Substring(0, Math.Min(700, body.Length)));
            // capture PSN token from auth responses (for the SDK's empty-Bearer race)
            if (r.Path.Contains(":auth")) {
                int ti = body.IndexOf("\"token\":\"");
                if (ti >= 0) {
                    int start = ti + 9;
                    int end = body.IndexOf('"', start);
                    if (end > start) {
                        LastPsnToken = body.Substring(start, end - start);
                        Log("~ captured PSN token (" + LastPsnToken.Length + " chars)");
                    }
                }
            }
            try {
                string safe = (r.Method + "_" + r.Path).Replace("/", "_").Replace(":", "-").Replace("?", "-");
                if (safe.Length > 100) safe = safe.Substring(0, 100);
                File.WriteAllText(Path.Combine(replayDir, safe + ".json"), body);
            } catch { }
            return true;
        } catch (Exception ex) { Log("~ proxy error: " + ex.Message); return false; }
    }

    // ---- minimal WebSocket/SignalR loop ----
    static int ReadExact(Stream s, byte[] buf, int n) { int got = 0; while (got < n) { int r = s.Read(buf, got, n - got); if (r <= 0) return got; got += r; } return n; }

    static void WsSend(Stream s, byte[] payload, byte opcode) {
        byte[] f = new byte[2 + payload.Length];
        f[0] = (byte)(0x80 | opcode); f[1] = (byte)payload.Length;
        Array.Copy(payload, 0, f, 2, payload.Length);
        s.Write(f, 0, f.Length); s.Flush();
    }

    static void WsLoop(Stream s) {
        bool handshook = false;
        System.Threading.Timer pingTimer = null;
        bool startDone = false, pushedSub = false;
        // ReceiveSubscriptionChangeMessage push: [1,{},nil,"ReceiveSubscriptionChangeMessage",[{status:"SUBSCRIBED",topic,entityId,entityType}]] (field names + SUBSCRIBED enum from PlayFabMultiplayerWin.dll)
        byte[] subPush = new byte[] { 0xbf,0x01,0x95,0x01,0x80,0xc0,0xd9,0x20,0x52,0x65,0x63,0x65,0x69,0x76,0x65,0x53,0x75,0x62,0x73,0x63,0x72,0x69,0x70,0x74,0x69,0x6f,0x6e,0x43,0x68,0x61,0x6e,0x67,0x65,0x4d,0x65,0x73,0x73,0x61,0x67,0x65,0x91,0x84,0xa6,0x73,0x74,0x61,0x74,0x75,0x73,0xaa,0x53,0x55,0x42,0x53,0x43,0x52,0x49,0x42,0x45,0x44,0xa5,0x74,0x6f,0x70,0x69,0x63,0xd9,0x43,0x31,0x7e,0x6c,0x6f,0x62,0x62,0x79,0x7e,0x4c,0x6f,0x62,0x62,0x79,0x43,0x68,0x61,0x6e,0x67,0x65,0x7e,0x63,0x30,0x63,0x61,0x62,0x36,0x33,0x65,0x2d,0x30,0x37,0x64,0x30,0x2d,0x34,0x35,0x30,0x35,0x2d,0x61,0x30,0x35,0x32,0x2d,0x66,0x65,0x32,0x62,0x37,0x39,0x39,0x64,0x31,0x39,0x34,0x35,0x2e,0x72,0x2d,0x32,0x30,0x32,0x36,0x30,0x33,0x32,0x33,0xa8,0x65,0x6e,0x74,0x69,0x74,0x79,0x49,0x64,0xb0,0x44,0x43,0x31,0x34,0x37,0x44,0x41,0x35,0x37,0x35,0x42,0x30,0x32,0x31,0x30,0x44,0xaa,0x65,0x6e,0x74,0x69,0x74,0x79,0x54,0x79,0x70,0x65,0xb4,0x74,0x69,0x74,0x6c,0x65,0x5f,0x70,0x6c,0x61,0x79,0x65,0x72,0x5f,0x61,0x63,0x63,0x6f,0x75,0x6e,0x74 };
        while (true) {
            byte[] h = new byte[2]; if (ReadExact(s, h, 2) < 2) return;
            int opcode = h[0] & 0x0F;
            int len = h[1] & 0x7F; bool masked = (h[1] & 0x80) != 0;
            if (len == 126) { byte[] e = new byte[2]; if (ReadExact(s, e, 2) < 2) return; len = (e[0] << 8) | e[1]; }
            else if (len == 127) { byte[] e = new byte[8]; if (ReadExact(s, e, 8) < 8) return; len = (int)BitConverter.ToUInt64(e, 0); }
            byte[] mask = new byte[4];
            if (masked && ReadExact(s, mask, 4) < 4) return;
            byte[] payload = new byte[len]; if (len > 0 && ReadExact(s, payload, len) < len) return;
            if (masked) for (int i = 0; i < len; i++) payload[i] ^= mask[i % 4];
            if (opcode == 8) { try { s.Write(new byte[] { 0x88, 0x00 }, 0, 2); } catch { } return; }   // close
            if (opcode == 9) { try { WsSend(s, payload, 0x0A); } catch { } continue; }               // ping -> pong
            if (opcode == 1 || opcode == 2) {                                                        // text/binary
                string txt = opcode == 1 ? Encoding.UTF8.GetString(payload)
                    : BitConverter.ToString(payload, 0, Math.Min(payload.Length, 400));
                Log("~ ws recv(" + opcode + "): " + txt.Substring(0, Math.Min(300, txt.Length)));
                if (!handshook) {
                    WsSend(s, Encoding.UTF8.GetBytes("{}\x1e"), 0x01);                                // SignalR handshake reply
                    handshook = true;
                    Log("~ ws SignalR handshake complete");
                    // keep-alive handled on THIS (loop) thread below to avoid concurrent SslStream writes; send one now
                    try { WsSend(s, new byte[] { 0x02, 0x91, 0x06 }, 0x02); } catch { }
                } else if (opcode == 2 && payload.Length > 6 && payload[1] == 0x96 && payload[2] == 0x01) {
                    // invocation [len][[1, {}, id, target, args, []]] -> Completion [3, {}, id, 3, result]
                    try {
                        int idLen = payload[4] & 0x1F;                          // fixstr invocationId
                        string invId = Encoding.ASCII.GetString(payload, 5, idLen);
                        int tPos = 5 + idLen;
                        int tLen = payload[tPos] & 0x1F;                          // fixstr target
                        string target = Encoding.ASCII.GetString(payload, tPos + 1, tLen);
                        Log("~ ws completing '" + target + "' id=" + invId);
                        string handle = "HD2LOCALCONNHANDLE00000001";              // StartOrRecoverSession -> connection handle
                        var msg = new List<byte>();
                        bool isStart = (target == "StartOrRecoverSession");
                        msg.Add(isStart ? (byte)0x95 : (byte)0x94);               // Completion: 5 elems (with result) or 4 (void)
                        msg.Add(0x03); msg.Add(0x80);                             // type=3 (Completion), {} headers
                        msg.Add((byte)(0xA0 | idLen)); msg.AddRange(Encoding.ASCII.GetBytes(invId));
                        if (isStart) {
                            // result object: { newConnectionHandle, oldConnectionHandle, recoveredTopics } (field names from PlayFabMultiplayerWin.dll)
                            msg.Add(0x03);                                        // ResultKind 3 = has result
                            msg.Add(0x83);                                        // fixmap, 3 entries
                            byte[] k1 = Encoding.ASCII.GetBytes("newConnectionHandle"); msg.Add((byte)(0xA0 | k1.Length)); msg.AddRange(k1);
                            byte[] hb = Encoding.ASCII.GetBytes(handle);               msg.Add((byte)(0xA0 | hb.Length)); msg.AddRange(hb);
                            byte[] k2 = Encoding.ASCII.GetBytes("oldConnectionHandle"); msg.Add((byte)(0xA0 | k2.Length)); msg.AddRange(k2);
                            msg.Add(0xA0);                                        // oldConnectionHandle = ""
                            byte[] k3 = Encoding.ASCII.GetBytes("recoveredTopics"); msg.Add((byte)(0xA0 | k3.Length)); msg.AddRange(k3);
                            msg.Add(0x90);                                        // recoveredTopics = []
                        } else {
                            msg.Add(0x02);                                        // ResultKind 2 = void (EndSession / AddEntityToSession / etc.)
                        }
                        byte[] comp = new byte[1 + msg.Count];
                        comp[0] = (byte)msg.Count; msg.CopyTo(comp, 1);           // varint length prefix (fits 1 byte)
                        WsSend(s, comp, 0x02);
                        if (isStart) startDone = true;
                    } catch (Exception ex) { Log("~ ws complete error: " + ex.Message); }
                }
                else {
                    try { WsSend(s, new byte[] { 0x02, 0x91, 0x06 }, 0x02); } catch { }   // server keepalive ping (loop-thread, thread-safe)
                    if (startDone && !pushedSub) { pushedSub = true; try { WsSend(s, subPush, 0x02); Log("~ ws -> ReceiveSubscriptionChangeMessage SUBSCRIBED"); } catch { } }   // confirm the lobby subscription the client just made
                }
            }
        }
    }

    // ---- raw pipe: byte-identical forwarding + real response capture (login only) ----
    static byte[] RawPipeLogin(Req r) {
        try {
            string host = "api.live.prod.thehelldiversgame.com";
            string ip = ResolveViaGoogle(host); if (ip == null) ip = ResolveRealIp(host);
            if (ip == null) { Log("~ rawpipe: no ip"); return null; }
            var tcp = new TcpClient();
            tcp.Connect(ip, 443);
            tcp.ReceiveTimeout = 15000; tcp.SendTimeout = 15000;
            var ssl = new SslStream(tcp.GetStream(), false, (snd, cert, chain, err) => true);
            try { ssl.AuthenticateAsClient(host, null, System.Security.Authentication.SslProtocols.Tls12, false); }
            catch (Exception ex2) {
                Log("~ rawpipe tls fail on " + ip + ": " + ex2.Message + " | inner: " + (ex2.InnerException != null ? ex2.InnerException.Message : "none"));
                tcp.Close();
                return null;
            }
            ssl.Write(r.RawBytes, 0, r.RawBytes.Length);
            ssl.Flush();
            Log("~ rawpipe: forwarded " + r.RawBytes.Length + " bytes verbatim");
            // read full response (headers + body via content-length; chunked fallback)
            var ms = new MemoryStream();
            byte[] b = new byte[65536];
            string headStr = ""; int headEnd = -1; int total = 0;
            while (headEnd < 0) {
                int n2 = ssl.Read(b, 0, b.Length);
                if (n2 <= 0) break;
                ms.Write(b, 0, n2); total += n2;
                headStr = Encoding.ASCII.GetString(ms.ToArray());
                headEnd = headStr.IndexOf("\r\n\r\n");
            }
            if (headEnd < 0) { Log("~ rawpipe: no header end"); tcp.Close(); return null; }
            int contentLen = 0;
            foreach (string hl in headStr.Substring(0, headEnd).Split(new[]{"\r\n"}, StringSplitOptions.None)) {
                if (hl.ToLower().StartsWith("content-length:")) int.TryParse(hl.Substring(15).Trim(), out contentLen);
            }
            byte[] all = ms.ToArray();
            while (all.Length - headEnd - 4 < contentLen) {
                int n2 = ssl.Read(b, 0, b.Length);
                if (n2 <= 0) break;
                ms.Write(b, 0, n2); all = ms.ToArray();
            }
            tcp.Close();
            byte[] resp = ms.ToArray();
            // --- RAW capture (gzip-safe): dump full response bytes verbatim, no string round-trip ---
            try {
                File.WriteAllBytes(Path.Combine(replayDir, "RAWPIPE_LoginResponse.bin"), resp);
                Log("~ rawpipe: saved raw " + resp.Length + "b -> RAWPIPE_LoginResponse.bin");
            } catch (Exception exRaw) { Log("~ rawpipe raw-save error: " + exRaw.Message); }
            Log("~ rawpipe: response head: " + headStr.Substring(0, Math.Min(headEnd, 700)).Replace("\r\n", " | "));
            string bodyStr = contentLen > 0 ? Encoding.UTF8.GetString(resp, headEnd + 4, Math.Min(contentLen, resp.Length - headEnd - 4)) : "";
            if (contentLen == 0 && headStr.ToLower().Contains("transfer-encoding: chunked")) {
                string raw = Encoding.UTF8.GetString(resp, headEnd + 4, resp.Length - headEnd - 4);
                bodyStr = DeChunk(raw);
            }
            Log("~ rawpipe: got " + resp.Length + "b resp, body(" + bodyStr.Length + ")=" + bodyStr.Substring(0, Math.Min(500, bodyStr.Length)));
            try { File.WriteAllText(Path.Combine(replayDir, "RAWPIPE_LoginResponse.json"), bodyStr); } catch { }
            return resp;
        } catch (Exception ex) { Log("~ rawpipe error: " + ex.Message); return null; }
    }

    static string DeChunk(string chunked) {
        var sb = new StringBuilder();
        int i = 0;
        while (i < chunked.Length) {
            int eol = chunked.IndexOf("\r\n", i);
            if (eol < 0) break;
            string sizeStr = chunked.Substring(i, eol - i).Trim();
            int size = Convert.ToInt32(sizeStr.Split(';')[0], 16);
            if (size == 0) break;
            i = eol + 2;
            if (i + size > chunked.Length) size = chunked.Length - i;
            sb.Append(chunked.Substring(i, size));
            i += size + 2;
        }
        return sb.ToString();
    }

    static void Route(Req r, out int status, out string body) {
        status = 200; body = "{}";
        string p = r.Path;
        string path = p.IndexOf('?') >= 0 ? p.Substring(0, p.IndexOf('?')) : p;
        string host = r.Headers.ContainsKey("host") ? r.Headers["host"] : "";

        // CAPTURE MODE: after a real (passthrough) login, flip this flag on. api.live.prod GETs are then
        // proxied to the REAL server (ProxyReal forwards the client's real "authorization: session <token>"
        // header; GETs are not X-Signature-signed so the real server accepts them) and the real response is
        // saved to a replay file. This harvests the exact FriendsV2/Operation/NewsFeed/etc. bodies.
        if (host.IndexOf("api.live.prod", StringComparison.OrdinalIgnoreCase) >= 0
            && (r.Method == "GET" || (r.Method == "POST" && path != "/api/Account/Login"))
            && File.Exists(@"C:\HD2VR\phase1\gateway\capture.flag")) {
            if (ProxyReal(r, out status, out body)) { Log("~ CAPTURED " + r.Path + " -> " + status + " (" + body.Length + "b)"); return; }
            Log("~ capture proxy failed for " + r.Path + ", falling through to fake");
        }

        // fake responses FIRST (before proxying)
        if (path == "/Client/LoginWithSteam") {
            string f = Path.Combine(replayDir, "POST__Client_LoginWithSteam.json");
            if (File.Exists(f)) { Log("< serving PlayFab login (captured REAL response)"); body = File.ReadAllText(f); return; }
            Log("< serving PlayFab login (handcrafted fallback)"); body = PlayFabLoginJson; return;
        }
        if (path == "/api/Account/Login") { Log("< serving Arrowhead login (John Helldiver)"); body = ArrowheadLoginJson; return; }
        if (path == "/api/Configuration/GameClient") {
            string f = Path.Combine(replayDir, "GET__api_Configuration_GameClient.json");
            if (File.Exists(f)) { Log("< serving GameClient config (captured real)"); body = File.ReadAllText(f); return; }
        }
        if (path.Contains("/gs_config/")) {
            string f = Path.Combine(replayDir, "GET__gs_config_v1_ns_helldivers2_config_main-data.json");
            if (File.Exists(f)) { Log("< serving gs_config (captured real)"); body = File.ReadAllText(f); return; }
        }

        // proxied hosts (real Sony/PSN)
        foreach (var ph in ProxyHosts) {
            if (host.Equals(ph, StringComparison.OrdinalIgnoreCase)) {
                if (ProxyReal(r, out status, out body)) return;
                body = "{}"; status = 200;
                return;
            }
        }
        if (path == "/Client/GetTitleNews") { Log("< GetTitleNews"); body = "{}"; return; }

        if (path.Equals("/api/WarSeason/current/WarId", StringComparison.OrdinalIgnoreCase)) { string f = Path.Combine(dataDir, "WarSeason_current_WarID.json"); if (File.Exists(f)) { Log("< war id"); body = File.ReadAllText(f); return; } }
        if (path.EndsWith("/timeSinceStart", StringComparison.OrdinalIgnoreCase)) {
            long val = 86157726 + (long)(DateTime.Now - new DateTime(2026, 10, 16, 20, 47, 19)).TotalSeconds;
            Log("< timeSinceStart dynamic " + val);
            body = "{\"secondsSinceStart\":" + val + "}";
            return;
        }
        if (path.EndsWith("/Status", StringComparison.OrdinalIgnoreCase)) {
            string f = Path.Combine(dataDir, "WarSeason_801_Status.json");
            if (File.Exists(f)) {
                // dynamic war clock: advance "time" from the captured snapshot so the war is live
                long baseTime = 84226280;
                long elapsed = (long)(DateTime.Now - new DateTime(2026, 10, 16, 20, 47, 19)).TotalSeconds;
                body = File.ReadAllText(f).Replace("\"time\":" + baseTime, "\"time\":" + (baseTime + elapsed));
                Log("< war status (time=" + (baseTime + elapsed) + ")");
                return;
            }
        }
        if (path.EndsWith("/WarInfo", StringComparison.OrdinalIgnoreCase)) { string f = Path.Combine(dataDir, "WarSeason_801_WarInfo.json"); if (File.Exists(f)) { Log("< war info"); body = File.ReadAllText(f); return; } }
        if (path.IndexOf("/NewsFeed", StringComparison.OrdinalIgnoreCase) >= 0) { string f = Path.Combine(dataDir, "NewsFeed_801.json"); if (File.Exists(f)) { Log("< newsfeed"); body = File.ReadAllText(f); return; } }

        // pubsub negotiate: serve captured response, point ws URL at the cert-covered host
        if (path == "/pubsub/negotiate") {
            string f = Path.Combine(replayDir, "POST__pubsub_negotiate-negotiateVersion=1.json");
            if (File.Exists(f)) { Log("< serving pubsub negotiate (url->westus3)"); body = File.ReadAllText(f).Replace("westus-001", "westus3-001"); return; }
        }

        // generic replay: serve captured real response if one exists for this path (query-suffixed name first)
        {
            string q = r.Path.IndexOf('?') >= 0 ? r.Path.Substring(r.Path.IndexOf('?') + 1) : "";
            string safeQ = (r.Method + "_" + path + (q.Length > 0 ? "-" + q : "")).Replace("/", "_").Replace(":", "-").Replace("?", "-");
            string safe = (r.Method + "_" + path).Replace("/", "_").Replace(":", "-").Replace("?", "-");
            foreach (string nm in new string[] { safeQ, safe }) {
                string n = nm.Length > 100 ? nm.Substring(0, 100) : nm;
                string f = Path.Combine(replayDir, n + ".json");
                if (File.Exists(f)) { Log("< replay " + n); body = File.ReadAllText(f); return; }
            }
        }

        Log("< DEFAULT 200 for " + path);
    }
}
