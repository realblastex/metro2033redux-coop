# Screenshot every metro.exe window into $out dir (half-size PNG). Restores iconic windows (they have a 0x0 client
# area) and uses PrintWindow(PW_RENDERFULLCONTENT) so the window doesn't need to be on top.
param([string]$out = "$env:TEMP")
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l,t,r,b; }
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
}
'@
foreach ($p in Get-Process metro -ErrorAction SilentlyContinue) {
  $h = $p.MainWindowHandle; if ($h -eq 0) { continue }
  $ic = [W]::IsIconic($h)
  if ($ic) { [W]::ShowWindow($h, 4) | Out-Null; Start-Sleep -Milliseconds 1500 }  # SW_SHOWNOACTIVATE
  $r = New-Object W+R; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.r - $r.l; $hh = $r.b - $r.t
  if ($w -le 0 -or $hh -le 0) { "$($p.Path): empty rect"; continue }
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
  $ok = [W]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
  $small = New-Object System.Drawing.Bitmap($bmp, [int]($w / 2), [int]($hh / 2))
  $f = Join-Path $out ("metro_" + (Split-Path (Split-Path $p.Path) -Leaf) + ".png")
  $small.Save($f); "$f ${w}x${hh} wasIconic=$ic printOk=$ok"
}
