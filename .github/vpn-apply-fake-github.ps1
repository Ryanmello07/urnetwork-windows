# A local stand-in for GitHub (vpn-apply-lib.ps1 Set-UrFakeGitHub), for the
# runner legs that need a release the official update helper will take but
# must not publish one: a failed install, a 3010 and an update by the official
# binaries. The hosts file sends api.github.com, github.com and the two
# release-asset hosts to 127.0.0.1, and this answers there over TLS with a
# certificate from a test CA in LocalMachine\Root, so everything the helper
# checks about the connection passes. It answers by Host:
#   api.github.com  /repositories/<RepoId>/releases?per_page=15
#                   200, the release list in -ListJson, with a Date header
#   github.com      /<owner>/<repo>/releases/download/<tag>/<asset>
#                   302 to release-assets.githubusercontent.com/<same path>
#   release-assets.githubusercontent.com, objects.githubusercontent.com
#                   200, the bytes of -Body
#   anything else   404
# One request per connection. Each request is logged with its host. Runs
# until it is stopped. Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0
param(
  [Parameter(Mandatory = $true)][string]$Thumbprint,
  [Parameter(Mandatory = $true)][string]$ListJson,
  [Parameter(Mandatory = $true)][string]$RepoId,
  [Parameter(Mandatory = $true)][string]$DownloadPath,
  [Parameter(Mandatory = $true)][string]$Body,
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

public static class UrFakeGitHub {
  public static void Serve(X509Certificate2 certificate, string listPath, string repoId, string downloadPath,
                           string bodyPath, string logPath) {
    byte[] list = File.ReadAllBytes(listPath);
    byte[] body = File.ReadAllBytes(bodyPath);
    string listRequest = "/repositories/" + repoId + "/releases?per_page=15";
    TcpListener listener = new TcpListener(IPAddress.Loopback, 443);
    listener.Start();
    File.AppendAllText(logPath, "listening on 127.0.0.1:443\n");
    while (true) {
      TcpClient client = listener.AcceptTcpClient();
      try {
        using (client)
        using (SslStream tls = new SslStream(client.GetStream(), false)) {
          tls.AuthenticateAsServer(certificate, false, SslProtocols.Tls12, false);
          string head = ReadHead(tls);
          string[] lines = head.Split(new string[] { "\r\n" }, StringSplitOptions.None);
          string host = "";
          foreach (string line in lines) {
            if (line.StartsWith("Host:", StringComparison.OrdinalIgnoreCase)) host = line.Substring(5).Trim().ToLowerInvariant();
          }
          string[] request = lines[0].Split(' ');
          string path = request.Length > 1 ? request[1] : "";
          string date = DateTime.UtcNow.ToString("r", CultureInfo.InvariantCulture);
          if (host == "api.github.com" && path == listRequest) {
            Respond(tls, "200 OK", "Content-Type: application/json\r\nDate: " + date + "\r\n", list);
            File.AppendAllText(logPath, "list: " + host + " " + path + " " + list.Length + " bytes\n");
          } else if (host == "github.com" && path == downloadPath) {
            string location = "https://release-assets.githubusercontent.com" + path;
            Respond(tls, "302 Found", "Location: " + location + "\r\nDate: " + date + "\r\n", new byte[0]);
            File.AppendAllText(logPath, "redirect: " + host + " " + path + " -> " + location + "\n");
          } else if ((host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com") &&
                     path == downloadPath) {
            Respond(tls, "200 OK", "Content-Type: application/octet-stream\r\nDate: " + date + "\r\n", body);
            File.AppendAllText(logPath, "served: " + host + " " + path + " " + body.Length + " bytes\n");
          } else {
            Respond(tls, "404 Not Found", "Date: " + date + "\r\n", new byte[0]);
            File.AppendAllText(logPath, "not found: " + host + " " + lines[0] + "\n");
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
  [UrFakeGitHub]::Serve($certificate, $ListJson, $RepoId, $DownloadPath, $Body, $Log)
} catch {
  Add-Content -LiteralPath $Log -Value "server failed: $($_.Exception.Message)"
  throw
}
