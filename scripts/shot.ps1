param([string]$out="C:\HD2VR\rework\hd2.png")
$code=@"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;
public class WinCap{
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 public static string Cap(IntPtr h,string p){
  RECT r; if(!GetWindowRect(h,out r)) return "norect";
  int w=r.R-r.L, ht=r.B-r.T; if(w<=0||ht<=0) return "bad";
  using(var b=new Bitmap(w,ht,PixelFormat.Format32bppArgb))
  using(var g=Graphics.FromImage(b)){
   IntPtr dc=g.GetHdc(); bool ok=PrintWindow(h,dc,2); g.ReleaseHdc(dc);
   int tw=1280, th=ht*tw/w;
   using(var s=new Bitmap(tw,th)) using(var g2=Graphics.FromImage(s)){ g2.DrawImage(b,0,0,tw,th); s.Save(p,ImageFormat.Png); }
   return "ok="+ok+" "+w+"x"+ht;
  }}}
"@
Add-Type -TypeDefinition $code -ReferencedAssemblies System.Drawing
$p=Get-Process helldivers2 -ErrorAction SilentlyContinue | Select-Object -First 1
if($p){ [WinCap]::Cap($p.MainWindowHandle,$out) } else { "helldivers2 not running" }
