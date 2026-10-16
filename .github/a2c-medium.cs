// What a process without administrator rights can do to the update channel,
// for A2c's runner legs: a2c-lib.ps1 builds this as a2c-medium.exe, a
// Windows program (no console, which a filtered token cannot open on the
// runner), and Start-A2cMedium starts it under a filtered token
// (Administrators deny-only, no privileges beyond a standard user's, Medium
// integrity). It writes what it found, one "key: value" line each, to its
// second argument.
//   probe <out> <paths>               its token, then for each path ('|'-separated):
//                                     whether its descriptor can be read, and which
//                                     of the rights below a handle opens with
//   hold <out> <paths> <mutex> <stop> as probe, then keeps open every handle it got,
//                                     sharing nothing, and a mutex it creates and
//                                     owns, until the file <stop> exists
//   press <out> <caption> <id>        presses button <id> of the top-level window
//                                     named <caption> as the runner does: UI
//                                     Automation's Invoke pattern, then the button's
//                                     BN_CLICKED posted to its dialog
// Nothing here synthesizes mouse or keyboard input.
// Throwaway (ci/a2c-legs* only).
//
// SPDX-License-Identifier: MPL-2.0
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading;

public static class A2cMediumProgram {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags,
                                   IntPtr template);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern IntPtr CreateMutexW(IntPtr security, bool initialOwner, string name);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string className, string title);
  [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr dialog, int id);
  [DllImport("user32.dll", SetLastError = true)] static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returned);
  [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
  [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);

  static readonly IntPtr Invalid = new IntPtr(-1);
  static string outPath;

  static void Say(string key, string value) { File.AppendAllText(outPath, key + ": " + value + "\r\n"); }

  // "<integrity> integrity, Administrators <enabled|not enabled>, user <name>"
  static string Token() {
    IntPtr token;
    if (!OpenProcessToken(GetCurrentProcess(), 0x0008, out token)) return "OpenProcessToken error " + Marshal.GetLastWin32Error();
    string level = "?";
    int size;
    GetTokenInformation(token, 25, IntPtr.Zero, 0, out size);
    IntPtr label = Marshal.AllocHGlobal(size);
    try {
      if (GetTokenInformation(token, 25, label, size, out size)) {
        IntPtr sid = Marshal.ReadIntPtr(label);
        int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
        int rid = Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
        level = rid == 0x2000 ? "Medium" : rid == 0x3000 ? "High" : rid == 0x4000 ? "System" : rid == 0x1000 ? "Low" : "0x" + rid.ToString("X");
      }
    } finally { Marshal.FreeHGlobal(label); }
    WindowsIdentity identity = WindowsIdentity.GetCurrent();
    bool admin = new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    return level + " integrity, Administrators " + (admin ? "enabled" : "not enabled") + ", user " + identity.Name;
  }

  static readonly string[] RightNames = {
    "READ_CONTROL", "FILE_READ_ATTRIBUTES", "SYNCHRONIZE", "FILE_READ_DATA", "FILE_WRITE_DATA", "FILE_APPEND_DATA",
    "FILE_WRITE_ATTRIBUTES", "DELETE", "WRITE_DAC", "WRITE_OWNER", "GENERIC_READ", "GENERIC_WRITE", "MAXIMUM_ALLOWED" };
  static readonly uint[] Rights = {
    0x00020000, 0x00000080, 0x00100000, 0x00000001, 0x00000002, 0x00000004,
    0x00000100, 0x00010000, 0x00040000, 0x00080000, 0x80000000, 0x40000000, 0x02000000 };

  static void Probe(string[] paths, bool hold, List<IntPtr> held) {
    foreach (string path in paths) {
      string name = Path.GetFileName(path);
      try {
        FileSecurity security = File.GetAccessControl(path);
        Say(name + " descriptor", security.GetSecurityDescriptorSddlForm(AccessControlSections.Owner | AccessControlSections.Access));
      } catch (Exception e) {
        Say(name + " descriptor", "not read: " + e.GetType().Name);
      }
      for (int i = 0; i < Rights.Length; i++) {
        // the hold keeps what it gets and shares nothing; the probe shares all.
        // OPEN_EXISTING; FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
        IntPtr handle = CreateFileW(path, Rights[i], hold ? 0u : 7u, IntPtr.Zero, 3, 0x02000000 | 0x00200000, IntPtr.Zero);
        if (handle == Invalid) {
          Say(name + " " + RightNames[i], "refused (" + Marshal.GetLastWin32Error() + ")");
        } else if (hold) {
          held.Add(handle);
          Say(name + " " + RightNames[i], "opened and held");
        } else {
          CloseHandle(handle);
          Say(name + " " + RightNames[i], "opened");
        }
      }
    }
  }

  [STAThread]
  public static int Main(string[] args) {
    if (args.Length < 2) return 2;
    outPath = args[1];
    try {
      Say("token", Token());
      Say("pid", System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
      string action = args[0];
      List<IntPtr> held = new List<IntPtr>();
      if (action == "probe" || action == "hold") {
        string[] paths = args.Length > 2 && args[2].Length > 0 ? args[2].Split('|') : new string[0];
        Probe(paths, action == "hold", held);
      }
      if (action == "hold") {
        string mutexName = args.Length > 3 ? args[3] : "";
        string stop = args.Length > 4 ? args[4] : "";
        if (mutexName.Length > 0) {
          IntPtr mutex = CreateMutexW(IntPtr.Zero, true, mutexName);
          int error = Marshal.GetLastWin32Error();
          if (mutex == IntPtr.Zero) Say("mutex", mutexName + " not created (" + error + ")");
          else if (error == 183) Say("mutex", mutexName + " opened, it existed already (183)");
          else Say("mutex", mutexName + " created and owned");
        }
        Say("holding", held.Count + " handles");
        DateTime deadline = DateTime.UtcNow.AddMinutes(30);
        while (!File.Exists(stop) && DateTime.UtcNow < deadline) Thread.Sleep(200);
        foreach (IntPtr handle in held) CloseHandle(handle);
        Say("released", held.Count + " handles");
      }
      if (action == "press") {
        string caption = args[2];
        int id = int.Parse(args[3]);
        IntPtr dialog = FindWindowW(null, caption);
        if (dialog == IntPtr.Zero) {
          Say("dialog", "not found");
        } else {
          Say("dialog", "found, window " + dialog);
          string uia = A2cUia.Invoke(dialog, args[3]);
          Say("uia", uia);
          if (uia != "invoked") {
            bool posted = PostMessageW(dialog, 0x0111, (IntPtr)id, GetDlgItem(dialog, id));
            Say("command", posted ? "posted" : "PostMessage error " + Marshal.GetLastWin32Error());
          }
        }
      }
      Say("done", "yes");
      return 0;
    } catch (Exception e) {
      Say("failed", e.GetType().Name + ": " + e.Message);
      return 1;
    }
  }
}
