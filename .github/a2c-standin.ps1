# A local stand-in for GitHub for A2c's runner legs (a2c-lib.ps1
# Start-A2cStandIn): both feeds' release lists and their downloads, from a
# routes file the test rewrites while the stand-in runs. The hosts file sends
# api.github.com, github.com and the two release-asset hosts to 127.0.0.1, and
# this answers there over TLS with a certificate from a test CA in
# LocalMachine\Root, so everything the update helper checks about the
# connection passes.
#
# The routes file is read again for every request. Its lines, tab-separated:
#   date   <the stand-in's date, RFC 1123>   <UTC ticks the clock runs from, 0 for a fixed date>
#   route  <host>  <path and query, exact>  <status>  <a file to serve, or a Location for a 302>  <seconds to wait first>
# A request no route names is answered 404. Every response carries the
# stand-in's date: the fixed one, or with ticks the date at that instant plus
# the time since. One request per connection. Each request is logged with its
# host, its path, in brackets its User-Agent (the update helper's is
# URnetwork-Windows-Update/<version>, the tray app's URnetwork-Windows/<version>),
# and the date it was answered with. Runs until it is stopped.
# Throwaway (ci/a2c-legs-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0
param(
  [Parameter(Mandatory = $true)][string]$Thumbprint,
  [Parameter(Mandatory = $true)][string]$Routes,
  [Parameter(Mandatory = $true)][string]$Log
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;

public static class A2cStandIn {
  class Route {
    public string Host, Path, Target;
    public int Status, Delay;
  }

  static string routesPath, logPath;

  static void Note(string line) {
    lock (typeof(A2cStandIn)) { File.AppendAllText(logPath, DateTime.UtcNow.ToString("HH:mm:ss.fff") + " " + line + "\n"); }
  }

  // The routes file as it is now, and the date to answer with.
  static Route Find(string host, string path, out string date) {
    string[] lines = null;
    for (int attempt = 0; attempt < 20 && lines == null; attempt++) {
      try { lines = File.ReadAllLines(routesPath); } catch (IOException) { Thread.Sleep(50); }
    }
    if (lines == null) throw new IOException("the routes file could not be read");
    date = DateTime.UtcNow.ToString("r", CultureInfo.InvariantCulture);
    Route found = null;
    foreach (string line in lines) {
      string[] f = line.Split('\t');
      if (f.Length >= 3 && f[0] == "date") {
        DateTime baseDate = DateTime.ParseExact(f[1], "r", CultureInfo.InvariantCulture,
                                                DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal);
        long ticks = long.Parse(f[2], CultureInfo.InvariantCulture);
        DateTime now = ticks == 0 ? baseDate : baseDate + (DateTime.UtcNow - new DateTime(ticks, DateTimeKind.Utc));
        date = now.ToString("r", CultureInfo.InvariantCulture);
      } else if (f.Length >= 6 && f[0] == "route" && found == null &&
                 string.Equals(f[1], host, StringComparison.OrdinalIgnoreCase) && f[2] == path) {
        found = new Route { Host = f[1], Path = f[2], Status = int.Parse(f[3], CultureInfo.InvariantCulture),
                            Target = f[4], Delay = int.Parse(f[5], CultureInfo.InvariantCulture) };
      }
    }
    return found;
  }

  public static void Serve(X509Certificate2 certificate, string routes, string log) {
    routesPath = routes;
    logPath = log;
    TcpListener listener = new TcpListener(IPAddress.Loopback, 443);
    listener.Start();
    // untimed, so Start-UrTestServer's wait for "^listening" sees it
    File.AppendAllText(logPath, "listening on 127.0.0.1:443, routes " + routes + "\n");
    while (true) {
      TcpClient client = listener.AcceptTcpClient();
      // each connection on its own thread, so a slow download does not hold
      // up a list request
      Thread worker = new Thread(delegate () { Answer(client, certificate); });
      worker.IsBackground = true;
      worker.Start();
    }
  }

  static void Answer(TcpClient client, X509Certificate2 certificate) {
    try {
      using (client)
      using (SslStream tls = new SslStream(client.GetStream(), false)) {
        tls.AuthenticateAsServer(certificate, false, SslProtocols.Tls12, false);
        string head = ReadHead(tls);
        string[] lines = head.Split(new string[] { "\r\n" }, StringSplitOptions.None);
        string host = "";
        string agent = "";
        foreach (string line in lines) {
          if (line.StartsWith("Host:", StringComparison.OrdinalIgnoreCase)) host = line.Substring(5).Trim().ToLowerInvariant();
          if (line.StartsWith("User-Agent:", StringComparison.OrdinalIgnoreCase)) agent = line.Substring(11).Trim();
        }
        string[] request = lines[0].Split(' ');
        string path = request.Length > 1 ? request[1] : "";
        string date;
        Route route = Find(host, path, out date);
        string who = " [" + agent + "] Date: " + date;
        if (route == null) {
          Respond(tls, "404 Not Found", "Date: " + date + "\r\n", new byte[0]);
          Note("not found: " + host + " " + path + who);
          return;
        }
        if (route.Delay > 0) {
          Note("waiting " + route.Delay + " s before answering " + host + " " + path + who);
          Thread.Sleep(route.Delay * 1000);
          Find(host, path, out date);
        }
        if (route.Status == 302) {
          Respond(tls, "302 Found", "Location: " + route.Target + "\r\nDate: " + date + "\r\n", new byte[0]);
          Note("redirect: " + host + " " + path + " -> " + route.Target + who);
        } else if (route.Status == 200) {
          byte[] body = File.ReadAllBytes(route.Target);
          string type = route.Target.EndsWith(".json", StringComparison.OrdinalIgnoreCase)
                            ? "application/json; charset=utf-8" : "application/octet-stream";
          Respond(tls, "200 OK", "Content-Type: " + type + "\r\nDate: " + date + "\r\n", body);
          Note("served: " + host + " " + path + " " + body.Length + " bytes of " + Path.GetFileName(route.Target) + who);
        } else {
          Respond(tls, route.Status + " Status", "Date: " + date + "\r\n", new byte[0]);
          Note("status " + route.Status + ": " + host + " " + path + who);
        }
      }
    } catch (Exception e) {
      Note("error: " + e.GetType().Name + ": " + e.Message);
    }
  }

  static void Respond(Stream tls, string status, string headers, byte[] payload) {
    byte[] head = Encoding.ASCII.GetBytes("HTTP/1.1 " + status + "\r\n" + headers + "Content-Length: " +
      payload.Length + "\r\nConnection: close\r\n\r\n");
    tls.Write(head, 0, head.Length);
    if (payload.Length > 0) tls.Write(payload, 0, payload.Length);
    tls.Flush();
  }

  static string ReadHead(Stream stream) {
    MemoryStream head = new MemoryStream();
    byte[] end = Encoding.ASCII.GetBytes("\r\n\r\n");
    int matched = 0;
    while (matched < end.Length && head.Length < 65536) {
      int next = stream.ReadByte();
      if (next < 0) break;
      head.WriteByte((byte)next);
      if (next == end[matched]) matched++;
      else matched = next == end[0] ? 1 : 0;
    }
    return Encoding.ASCII.GetString(head.ToArray());
  }
}
'@

$certificate = Get-Item "Cert:\LocalMachine\My\$Thumbprint"
if (-not $certificate.HasPrivateKey) { throw "certificate $Thumbprint has no private key" }
try {
  [A2cStandIn]::Serve($certificate, $Routes, $Log)
} catch {
  Add-Content -LiteralPath $Log -Value "server failed: $($_.Exception.Message)"
  throw
}
