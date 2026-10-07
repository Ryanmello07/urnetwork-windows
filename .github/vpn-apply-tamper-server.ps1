# The tampered leg's HTTPS server (vpn-apply-lib.ps1 Set-UrTamperedDownload).
# It answers every request on 127.0.0.1:443 with one file, over TLS with a
# certificate from LocalMachine\My, and logs each request line and its host.
# Runs until it is stopped. Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0
param(
  [Parameter(Mandatory = $true)][string]$Thumbprint,
  [Parameter(Mandatory = $true)][string]$Body,
  [Parameter(Mandatory = $true)][string]$Log
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;

public static class UrTamperServer {
  public static void Serve(X509Certificate2 certificate, string bodyPath, string logPath) {
    byte[] body = File.ReadAllBytes(bodyPath);
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
            if (line.StartsWith("Host:", StringComparison.OrdinalIgnoreCase)) host = line.Substring(5).Trim();
          }
          File.AppendAllText(logPath, "request: " + lines[0] + " host " + host + "\n");
          byte[] header = Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n" +
            "Content-Length: " + body.Length + "\r\nConnection: close\r\n\r\n");
          tls.Write(header, 0, header.Length);
          tls.Write(body, 0, body.Length);
          tls.Flush();
          File.AppendAllText(logPath, "served " + body.Length + " bytes\n");
        }
      } catch (Exception e) {
        File.AppendAllText(logPath, "error: " + e.GetType().Name + ": " + e.Message + "\n");
      }
    }
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
  [UrTamperServer]::Serve($certificate, $Body, $Log)
} catch {
  Add-Content -LiteralPath $Log -Value "server failed: $($_.Exception.Message)"
  throw
}
