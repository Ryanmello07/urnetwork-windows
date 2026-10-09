# A local stand-in for GitHub (vpn-feed-lib.ps1 Set-UrFeedStandIn) that serves
# the official feed's own release list, as GitHub returned it, with a Date
# header of the caller's choosing: the one thing the test changes is what day
# the list says it is. The hosts file sends api.github.com, github.com and the
# two release-asset hosts to 127.0.0.1, and this answers there over TLS with a
# certificate from a test CA in LocalMachine\Root, so everything the update
# helper checks about the connection passes. It answers by Host:
#   api.github.com  <ListPath>, exactly
#                   200, the bytes of -ListJson, Date: <the stand-in's date>
#   github.com      <DownloadPath>
#                   302 to release-assets.githubusercontent.com/<same path>
#   release-assets.githubusercontent.com, objects.githubusercontent.com
#                   <DownloadPath>: 200, the bytes of -Body
#   anything else   404
# Every response carries the stand-in's date. With -ClockStartTicks 0 that is
# -DateHeader, the same for every response. With the UTC ticks of an instant
# it is a running clock: -DateHeader at that instant, and one second later
# every second after it, so a test can let the stand-in's day change while the
# app runs, as GitHub's does. One request per connection.
# Each request is logged with its host, in brackets its User-Agent (the update
# helper's is URnetwork-Windows-Update/<version>, the tray app's
# URnetwork-Windows/<version>), and the date it was answered with. Runs until
# it is stopped. Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0
param(
  [Parameter(Mandatory = $true)][string]$Thumbprint,
  [Parameter(Mandatory = $true)][string]$ListJson,
  [Parameter(Mandatory = $true)][string]$ListPath,
  [Parameter(Mandatory = $true)][string]$DateHeader,
  [Parameter(Mandatory = $true)][string]$DownloadPath,
  [Parameter(Mandatory = $true)][string]$Body,
  [Parameter(Mandatory = $true)][string]$Log,
  [string]$ClockStartTicks = '0'
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

public static class UrFeedStandIn {
  static DateTime baseDate;
  static long clockStartTicks;

  // The date every response carries: the fixed one, or the running clock's.
  static string Now() {
    DateTime date = baseDate;
    if (clockStartTicks != 0) date = baseDate + (DateTime.UtcNow - new DateTime(clockStartTicks, DateTimeKind.Utc));
    return date.ToString("r", CultureInfo.InvariantCulture);
  }

  public static void Serve(X509Certificate2 certificate, string listPath, string listRequest, string date,
                           long startTicks, string downloadPath, string bodyPath, string logPath) {
    byte[] list = File.ReadAllBytes(listPath);
    byte[] body = File.ReadAllBytes(bodyPath);
    baseDate = DateTime.ParseExact(date, "r", CultureInfo.InvariantCulture,
                                   DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal);
    clockStartTicks = startTicks;
    TcpListener listener = new TcpListener(IPAddress.Loopback, 443);
    listener.Start();
    File.AppendAllText(logPath, "listening on 127.0.0.1:443, Date: " + Now() +
                                (startTicks != 0 ? " (a running clock)" : " (fixed)") + "\n");
    while (true) {
      TcpClient client = listener.AcceptTcpClient();
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
          string now = Now();
          string who = " [" + agent + "] Date: " + now + "\n";
          if (host == "api.github.com" && path == listRequest) {
            Respond(tls, "200 OK", "Content-Type: application/json; charset=utf-8\r\nDate: " + now + "\r\n", list);
            File.AppendAllText(logPath, "list: " + host + " " + path + " " + list.Length + " bytes" + who);
          } else if (host == "github.com" && path == downloadPath) {
            string location = "https://release-assets.githubusercontent.com" + path;
            Respond(tls, "302 Found", "Location: " + location + "\r\nDate: " + now + "\r\n", new byte[0]);
            File.AppendAllText(logPath, "redirect: " + host + " " + path + " -> " + location + who);
          } else if ((host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com") &&
                     path == downloadPath) {
            Respond(tls, "200 OK", "Content-Type: application/octet-stream\r\nDate: " + now + "\r\n", body);
            File.AppendAllText(logPath, "served: " + host + " " + path + " " + body.Length + " bytes" + who);
          } else {
            Respond(tls, "404 Not Found", "Date: " + now + "\r\n", new byte[0]);
            File.AppendAllText(logPath, "not found: " + host + " " + lines[0] + who);
          }
        }
      } catch (Exception e) {
        File.AppendAllText(logPath, "error: " + e.GetType().Name + ": " + e.Message + "\n");
      }
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
  [UrFeedStandIn]::Serve($certificate, $ListJson, $ListPath, $DateHeader, [long]$ClockStartTicks, $DownloadPath, $Body, $Log)
} catch {
  Add-Content -LiteralPath $Log -Value "server failed: $($_.Exception.Message)"
  throw
}
